#include "render/render.hpp"

#include "mujoco.h"
#include <Eigen/Dense>

#include <iostream>
#include <thread>
#include <chrono>
#include <utility>
#include <set>

// Globals
static mjModel* m;
static mjData* d;

static std::mutex mtx;

// Convenience typedefs for interop between mujoco and eigen
typedef Eigen::Map<
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> mjMapMatrix_t;

typedef Eigen::Map<
    Eigen::Matrix<double, Eigen::Dynamic, 1>> mjMapVector_t;


// TODO: better name
// Avoids contention with code that edits/reads global mjData in seperate threads
// (e.g. the rendering thread), provided they share the same mutex
#define THREADSAFE(mj_func)      \
do {                             \
    mtx.lock();                  \
    mj_func;                     \
    mtx.unlock();                \
} while(0)


#define PRINTDIMS(mat) \
do {                   \
    std::cout << #mat << ": " << mat.rows() << " x " << mat.cols() << std::endl; \
} while(0)


void activate_mujoco()
{
    std::string mjkeyPath;
    
    char *home = std::getenv("HOME");
    if (!home) {
        std::cerr << "Error: home directory not found" << std::endl;
        exit(-1);
    }

    mjkeyPath += std::string(home) + "/.mujoco/mjkey.txt";
    mj_activate(mjkeyPath.c_str());

    char error[1000] = "";
    m = mj_loadXML("assets/4bar_actuated.xml", NULL, error, 1000);

    if (!m) {
        std::cerr << error << std::endl;
        exit(-1);
    }

    d = mj_makeData(m);
}


// Splits dof indices into independent and dependent dofs
// "Independent" is (arbitrarily) defined relative to actuated joints
std::pair<Eigen::VectorXi, Eigen::VectorXi> split_idcs(const mjModel *m)
{ 
    Eigen::VectorXi ind_idx(m->nu);
    Eigen::VectorXi dep_idx(m->nv - m->nu);

    int i = 0, d = 0;
    for(int dof = 0; dof < m->nv; dof++){
        int jnt = m->dof_jntid[dof];
        auto name = mj_id2name(m, mjOBJ_JOINT, jnt);

        // TODO?: Assumes actuator names correspond to joint names
        // Could use trnid instead

        // Joint is independent (floating base || actuated || spring)
        if (name == nullptr || mj_name2id(m, mjOBJ_ACTUATOR, name) != -1 || m->jnt_stiffness[jnt] > 0) 
            ind_idx[i++] = dof;
        
        else 
            dep_idx[d++] = dof;
    }

    return std::make_pair(ind_idx, dep_idx);
}


Eigen::MatrixXd getSubmatrix(Eigen::VectorXi col_idcs, Eigen::MatrixXd mat)
{
    // TODO: avoid copies
    Eigen::MatrixXd submat = Eigen::MatrixXd::Zero(mat.rows(), col_idcs.size());

    for (int i = 0; i < col_idcs.size(); i++) 
        submat.col(i) = mat.col(col_idcs[i]);
    
    return submat;
}


Eigen::MatrixXd getInertiaMatrix(const mjModel *m, const mjData* d)
{
    mjtNum M[m->nv * m->nv];
    THREADSAFE(mj_fullM(m, M, d->qM));
    return mjMapMatrix_t(M, m->nv, m->nv); // not threadsafe
}


Eigen::VectorXd getBiasVector(const mjModel *m, mjData* d)
{
    THREADSAFE(mj_forward(m, d));
    return mjMapVector_t(d->qfrc_bias, m->nv); // not threadsafe
}


// solves [q_ind q_dep] = gamma * q_dep 
Eigen::MatrixXd getConstraintProjectionMatrix(const mjModel *m, mjData* d, Eigen::MatrixXd G)
{
    Eigen::VectorXi ind_idx, dep_idx;
    std::tie(ind_idx, dep_idx) = split_idcs(m);

    auto G_ind = getSubmatrix(ind_idx, G);
    auto G_dep = getSubmatrix(dep_idx, G);

    auto K = -G_dep.inverse() * G_ind;

    Eigen::MatrixXd gamma = Eigen::MatrixXd::Identity(2 * K.rows(), K.cols());
    gamma.bottomRows(K.rows()) = K;

    Eigen::VectorXi order(ind_idx.size() + dep_idx.size());
    order << ind_idx, dep_idx;

    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic> P(order);
    
    return P * gamma; // Permute gamma so it's in original q order
}


Eigen::MatrixXd getConstraintJacobian(const mjModel* m, const mjData* d)
{
    Eigen::MatrixXd G;
    THREADSAFE(
        G = mjMapMatrix_t(d->efc_J, m->neq*3, m->nv);
    );

    // Zero out negligible entries for numerical stability
    G = (G.array().abs() < 1e-4).select(0, G); 

    return G;
}


// For a rank deficient matrix M this function returns a 
// matrix R that is row equivalent to M and has full rank
Eigen::MatrixXd getFullRankRowEquivalent(Eigen::MatrixXd M)
{
    // TODO: investigate using QR decomp

    // Get M = P^(-1)LUQ^(-1) decomposition
    Eigen::FullPivLU<Eigen::MatrixXd> lu(M);

    // Extract R = UQ^(-1), which is row equivalent to M
    Eigen::MatrixXd U = lu.matrixLU().triangularView<Eigen::Upper>();
    Eigen::MatrixXd R = U * lu.permutationQ().inverse();

    // Note: U is rank revealing (all rows are linearly dependent or 0)
    // and also sorted so the "smallest" rows are last

    // Get rid of zero rows so U is no longer rank deficient
    // U is sorted so we only need to check bottom (m - rank(U)) rows
    // Calling lu.rank() directly won't account for numerical precision
    int rank_estimate = R.rows();
    while(R.row(rank_estimate - 1).isZero(1e-5))
        rank_estimate -= 1;

    return R.block(0, 0, rank_estimate, R.cols());
}


Eigen::MatrixXd constrainedInverseDynamics(const mjModel *m, mjData* d)
{
    // No desired acceleration
    auto v_dot = Eigen::VectorXd::Zero(m->nv);

    auto G = getConstraintJacobian(m, d);
    auto R = getFullRankRowEquivalent(G);

    auto gamma = getConstraintProjectionMatrix(m, d, R);
    auto M = getInertiaMatrix(m, d);
    auto c = getBiasVector(m, d);

    auto tau = M * v_dot + c;
    auto u = gamma.transpose() * tau;

    return u;
}


void inverseDynamics(const mjModel *m, mjData* d)
{
    mjMapVector_t q(d->qpos, m->nq);
    mjMapVector_t v(d->qvel, m->nv);
    mjMapVector_t v_dot(d->qacc, m->nv);

    mjMapVector_t tau(d->qfrc_inverse, m->nv);

    Eigen::VectorXd v_dot_cached;

    THREADSAFE(
        //v_dot_cached = v_dot;
        v_dot.setZero(); // No desired acceleration
        mj_inverse(m, d);
        //v_dot = v_dot_cached;
    );

    mjMapVector_t tau_applied(d->qfrc_applied, m->nv);

    auto M = getInertiaMatrix(m, d);
    auto c = getBiasVector(m, d);

    // Equivalent
    // Eigen::MatrixXd tau_id = M * v_dot + c;

    tau_applied = tau;

    std::cout << tau.transpose() << std::endl;
}


int main() 
{
    std::mutex mtx;
    activate_mujoco();

    // double qpos_init[] = { 
    //      0.0045, 0, 0.4973, 0.9785, -0.0164,  0.0178, -0.2049,
    //     -1.1997, 0, 1.4267, 0,      -1.5244,  1.5244, -1.5968,
    //     -0.0045, 0, 0.4973, 0.9786,  0.0038, -0.0152, -0.2051,
    //     -1.1997, 0, 1.4267, 0,      -1.5244,  1.5244, -1.5968
    // };
    // mju_copy(&d->qpos[7], qpos_init, 28);

    // double qpos_init[] = {
    // 3.52662e-05, 6.6877e-06, 1.01002, 
    // 1, 0.000261766, -0.000213897, -3.32552e-05,-0.000457461,0.00038115,
    // 0.000398052,0.999996,5.62197e-05,0.00011356,-0.00272258,-0.785047,
    // -0.00814102,1.02407,-0.00842223,-0.000112123,-0.000141954,-0.00155376
    // };

    // mju_copy(&d->qpos[0], qpos_init, 20);

    // Eigen::PermutationMatrix<4, 4> perm;
    // perm.indices() = {0, 3, 1, 2};

    // Eigen::Vector4d vec(0, 3, 1, 2);

    // std::cout << vec.transpose() << std::endl;
    // std::cout << (perm.transpose() * vec).transpose() << std::endl;

    // exit(1);

    mj_forward(m, d);
    double weight = mj_getTotalmass(m) * 9.806;

    mjMapVector_t u(d->ctrl, m->nu);

    std::thread render_thread(render, m, d, std::ref(mtx));

    // Give some time for the rendering thread to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int ts = static_cast<int>(1000 * m->opt.timestep);
    std::chrono::milliseconds timestep(ts);

    // This will slowly diverge from realtime, since sleep_until 
    // is inexact, but that's fine for our purposes
    while (true) {
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + timestep;

        u = constrainedInverseDynamics(m, d);

        //inverseDynamics(m, d);

        //Eigen::Vector3d x_force = {0.0, 0.0, weight};
        //get_control(m, d, Eigen::VectorXd::Zero(m->nv), x_force, mtx);

        THREADSAFE(mj_step(m, d));

        std::this_thread::sleep_until(end_time);
    }

    render_thread.join();

    mj_deleteData(d); 
    mj_deleteModel(m);
}
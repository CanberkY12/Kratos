// KRATOS  ___|  |                   |                   |
//       \___ \  __|  __| |   |  __| __| |   |  __| _` | |
//             | |   |    |   | (    |   |   | |   (   | |
//       _____/ \__|_|   \__,_|\___|\__|\__,_|_|  \__,_|_| MECHANICS
//
//  License:		 BSD License
//					 license: structural_mechanics_application/license.txt
//
//  Main authors:
//

// System includes
#include <functional>
#include <stack>
#include <string>
#include <unordered_map>

// External includes

// Project includes
#include "containers/model.h"
#include "includes/shared_pointers.h"
#include "linear_solvers/skyline_lu_custom_scalar_solver.h"
#include "solving_strategies/convergencecriterias/residual_criteria.h"
#include "solving_strategies/schemes/residual_based_adjoint_bossak_scheme.h"
#include "solving_strategies/schemes/residual_based_bossak_displacement_scheme.hpp"
#include "solving_strategies/strategies/residualbased_linear_strategy.h"
#include "solving_strategies/strategies/residualbased_newton_raphson_strategy.h"
#include "spaces/ublas_space.h"
#include "testing/testing.h"
#include "utilities/sensitivity_builder.h"

// Application includes
#include "custom_response_functions/response_utilities/adjoint_nodal_displacement_response_function.h"
#include "tests/cpp_tests/bits/adjoint_test_model_part_factory.h"
#include "tests/cpp_tests/bits/test_model_part_factory.h"

namespace Kratos
{
namespace
{
namespace smtsts
{ // cotire unity guard
struct PrimalTestSolver
{
    std::function<ModelPart&()> ModelPartFactory;
    unsigned ResponseNodeId;
    double TotalTime;
    double TimeStepSize;

    double CalculateResponseValue();

    double CalculateResponseValue(unsigned NodeToPerturb, char Direction, double Perturbation);
};

struct AdjointTestSolver
{
    std::function<ModelPart&()> ModelPartFactory;
    unsigned ResponseNodeId;
    double TotalTime;
    double TimeStepSize;

    double CalculateSensitivity(unsigned NodeToPerturb, char Direction);
};

} // namespace smtsts
} // namespace

namespace Testing
{
KRATOS_TEST_CASE_IN_SUITE(TotalLagrangian2D3_TransientSensitivity, KratosStructuralMechanicsFastSuite)
{
    Model this_model;
    auto model_part_factory = [&this_model]() -> ModelPart& {
        return CreateStructuralMechanicsTestModelPart(
            &this_model, KratosComponents<Element>::Get("TotalLagrangianElement2D3N"),
            KratosComponents<ConstitutiveLaw>::Get(
                "LinearElasticPlaneStrain2DLaw"),
            [](ModelPart* pModelPart) {
                pModelPart->GetNode(1).Fix(DISPLACEMENT_X);
                pModelPart->GetNode(1).Fix(DISPLACEMENT_Y);
                pModelPart->GetNode(3).Fix(DISPLACEMENT_X);
                pModelPart->GetNode(3).Fix(DISPLACEMENT_Y);
            });
    };
    const double time_step_size = 0.004;
    const double total_time = 0.015; // approx. 1/4 of a period.
    const unsigned response_node_id = 2;
    smtsts::PrimalTestSolver solver{model_part_factory, response_node_id,
                                    total_time, time_step_size};
    smtsts::AdjointTestSolver adjoint_solver{
        model_part_factory, response_node_id, total_time, time_step_size};
    const double delta = 1e-5;
    const double response_value0 = solver.CalculateResponseValue();
    for (unsigned i_node : {1, 2, 3})
    {
        for (char dir : {'x', 'y'})
        {
            const double response_value1 =
                solver.CalculateResponseValue(i_node, dir, delta);
            const double finite_diff_sensitivity =
                -(response_value1 - response_value0) / delta;
            const double adjoint_sensitivity =
                adjoint_solver.CalculateSensitivity(i_node, dir);
            KRATOS_CHECK_NEAR(adjoint_sensitivity, finite_diff_sensitivity, 1e-9);
        }
    }
}
} // namespace Testing

namespace // cpp internals
{
namespace smtsts
{ // cotire unity guard
typedef TUblasSparseSpace<double> SparseSpaceType;
typedef TUblasDenseSpace<double> LocalSpaceType;
typedef LinearSolver<SparseSpaceType, LocalSpaceType> LinearSolverType;
typedef Scheme<SparseSpaceType, LocalSpaceType> SchemeType;
typedef ConvergenceCriteria<SparseSpaceType, LocalSpaceType> ConvergenceCriteriaType;
typedef SolvingStrategy<SparseSpaceType, LocalSpaceType, LinearSolverType> SolvingStrategyType;

AdjointResponseFunction::Pointer ResponseFunctionFactory(ModelPart* pModelPart, unsigned ResponseNodeId)
{
    Parameters params{R"({"traced_dof": "DISPLACEMENT_Y", "gradient_mode": "semi_analytic", "step_size": 1e-2})"};
    params.AddEmptyValue("traced_node_id");
    params["traced_node_id"].SetInt(ResponseNodeId);
    return Kratos::make_shared<AdjointNodalDisplacementResponseFunction>(*pModelPart, params);
}

SolvingStrategyType::Pointer CreatePrimalSolvingStrategy(ModelPart* pModelPart)
{
    LinearSolverType::Pointer p_linear_solver =
        Kratos::make_shared<SkylineLUCustomScalarSolver<SparseSpaceType, LocalSpaceType>>();
    SchemeType::Pointer p_scheme =
        Kratos::make_shared<ResidualBasedBossakDisplacementScheme<SparseSpaceType, LocalSpaceType>>(0.0);
    ConvergenceCriteriaType::Pointer p_conv_criteria =
        Kratos::make_shared<ResidualCriteria<SparseSpaceType, LocalSpaceType>>(1e-12, 1e-14);
    return Kratos::make_shared<ResidualBasedNewtonRaphsonStrategy<SparseSpaceType, LocalSpaceType, LinearSolverType>>(
        *pModelPart, p_scheme, p_linear_solver, p_conv_criteria, 30, true, false, true);
}

void SolvePrimal(ModelPart* pModelPart,
                 double DeltaTime,
                 double TotalTime,
                 std::function<void(ModelPart*)> CallerFun)
{
    auto p_solver = CreatePrimalSolvingStrategy(pModelPart);
    p_solver->Initialize();
    const double start_time = 0.0;
    pModelPart->CloneTimeStep(start_time - DeltaTime);
    pModelPart->CloneTimeStep(start_time);
    for (double current_time = start_time; current_time < TotalTime;)
    {
        current_time += DeltaTime;
        pModelPart->CloneTimeStep(current_time);
        p_solver->Solve();
        CallerFun(pModelPart);
    }
}

double CalculateResponseValue(ModelPart* pModelPart, double DeltaTime, double TotalTime, unsigned ResponseNodeId)
{
    auto p_response_function = ResponseFunctionFactory(pModelPart, ResponseNodeId);
    p_response_function->Initialize();
    double response_value = 0.;
    auto sum_response_value = [&](ModelPart* pModelPart) {
        response_value += pModelPart->GetProcessInfo()[DELTA_TIME] *
                          p_response_function->CalculateValue(*pModelPart);
    };
    SolvePrimal(pModelPart, DeltaTime, TotalTime, sum_response_value);
    return response_value;
}

double PrimalTestSolver::CalculateResponseValue()
{
    ModelPart& primal_model_part = ModelPartFactory();
    return smtsts::CalculateResponseValue(&primal_model_part, TimeStepSize,
                                          TotalTime, ResponseNodeId);
}

double PrimalTestSolver::CalculateResponseValue(unsigned NodeToPerturb, char Direction, double Perturbation)
{
    KRATOS_ERROR_IF(Direction != 'x' && Direction != 'y')
        << "invalid direction: '" << Direction << "'";
    KRATOS_ERROR_IF(Perturbation <= 0.) << "invalid perturbation: " << Perturbation;
    ModelPart& primal_model_part = ModelPartFactory();
    const unsigned i_dir = (Direction == 'x') ? 0 : 1;
    primal_model_part.GetNode(NodeToPerturb).GetInitialPosition()[i_dir] += Perturbation;
    return smtsts::CalculateResponseValue(&primal_model_part, TimeStepSize,
                                          TotalTime, ResponseNodeId);
}

SolvingStrategyType::Pointer CreateAdjointSolvingStrategy(ModelPart* pModelPart,
                                                          AdjointResponseFunction::Pointer pResponseFunction)
{
    LinearSolverType::Pointer p_linear_solver =
        Kratos::make_shared<SkylineLUCustomScalarSolver<SparseSpaceType, LocalSpaceType>>();
    Parameters settings(R"({ "alpha_bossak": 0.0 })");
    SchemeType::Pointer p_adjoint_scheme =
        Kratos::make_shared<ResidualBasedAdjointBossakScheme<SparseSpaceType, LocalSpaceType>>(
            settings, pResponseFunction);
    return Kratos::make_shared<ResidualBasedLinearStrategy<SparseSpaceType, LocalSpaceType, LinearSolverType>>(
        *pModelPart, p_adjoint_scheme, p_linear_solver);
}

struct PrimalSolution
{
    using NodeIdType = unsigned;
    double time;
    std::unordered_map<NodeIdType, array_1d<double, 3>> disp;
    std::unordered_map<NodeIdType, array_1d<double, 3>> vel;
    std::unordered_map<NodeIdType, array_1d<double, 3>> acc;
};

void SolveAdjoint(ModelPart* pAdjointModelPart,
                  std::stack<PrimalSolution>* pPrimalSolution,
                  unsigned ResponseNodeId)
{
    auto p_response_function = ResponseFunctionFactory(pAdjointModelPart, ResponseNodeId);
    auto p_adjoint_solver =
        CreateAdjointSolvingStrategy(pAdjointModelPart, p_response_function);
    p_adjoint_solver->Initialize();
    SensitivityBuilder sensitivity_builder(
        Parameters(R"({"nodal_solution_step_sensitivity_variables": ["SHAPE_SENSITIVITY"]})"),
        *pAdjointModelPart, p_response_function);
    sensitivity_builder.Initialize();
    while (!pPrimalSolution->empty())
    {
        auto p_sol = &pPrimalSolution->top();
        pAdjointModelPart->CloneTimeStep(p_sol->time);
        for (auto& r_adjoint_node : pAdjointModelPart->Nodes())
        {
            r_adjoint_node.FastGetSolutionStepValue(DISPLACEMENT) =
                p_sol->disp[r_adjoint_node.Id()];
            r_adjoint_node.X() = r_adjoint_node.X0() +
                                 r_adjoint_node.FastGetSolutionStepValue(DISPLACEMENT_X);
            r_adjoint_node.Y() = r_adjoint_node.Y0() +
                                 r_adjoint_node.FastGetSolutionStepValue(DISPLACEMENT_Y);
            r_adjoint_node.FastGetSolutionStepValue(VELOCITY) =
                p_sol->vel[r_adjoint_node.Id()];
            r_adjoint_node.FastGetSolutionStepValue(ACCELERATION) =
                p_sol->acc[r_adjoint_node.Id()];
        }
        p_adjoint_solver->Solve();
        sensitivity_builder.UpdateSensitivities();
        pPrimalSolution->pop();
    }
}

double AdjointTestSolver::CalculateSensitivity(unsigned NodeToPerturb, char Direction)
{
    KRATOS_ERROR_IF(Direction != 'x' && Direction != 'y')
        << "invalid direction: '" << Direction << "'";
    ModelPart& primal_model_part = ModelPartFactory();
    std::stack<PrimalSolution> sol;
    auto record_primal = [&](ModelPart* pModelPart) {
        sol.push(PrimalSolution{pModelPart->GetProcessInfo()[TIME]});
        auto& current_sol = sol.top();
        for (const auto& r_node : pModelPart->Nodes())
        {
            current_sol.disp[r_node.Id()] = r_node.FastGetSolutionStepValue(DISPLACEMENT);
            current_sol.vel[r_node.Id()] = r_node.FastGetSolutionStepValue(VELOCITY);
            current_sol.acc[r_node.Id()] = r_node.FastGetSolutionStepValue(ACCELERATION);
        };
    };
    SolvePrimal(&primal_model_part, TimeStepSize, TotalTime, record_primal);
    ModelPart& adjoint_model_part =
        CreateStructuralMechanicsAdjointTestModelPart(&primal_model_part);
    SolveAdjoint(&adjoint_model_part, &sol, ResponseNodeId);
    const unsigned i_dir = (Direction == 'x') ? 0 : 1;
    return adjoint_model_part.GetNode(NodeToPerturb).FastGetSolutionStepValue(SHAPE_SENSITIVITY)[i_dir];
}

} // namespace smtsts
} // namespace
} // namespace Kratos
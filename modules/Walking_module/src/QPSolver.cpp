

// iDynTree
#include <iDynTree/Core/EigenHelpers.h>
#include <iDynTree/Core/EigenSparseHelpers.h>

#include <QPSolver.hpp>
#include <Utils.hpp>

QPSolver::QPSolver(const int& inputSize,
                   const int& numberOfAllConstraints)
    :m_inputSize(inputSize)
{
    // instantiate the solver class
    m_QPSolver = std::make_unique<OsqpEigen::Solver>();


    //set the number of deceision variables of QP problem
    m_QPSolver->data()->setNumberOfVariables(inputSize);

    // set the number of all constraints includes inequality and equality constraints
    m_QPSolver->data()->setNumberOfConstraints(numberOfAllConstraints);

    m_QPSolver->settings()->setVerbosity(false);
}




bool QPSolver::setHessianMatrix(const iDynTree::Vector4& alphaVector ){

    Eigen::SparseMatrix<double> Hessian;
    Hessian.resize(3,3);
    Hessian.reserve(5);
    Hessian.insert(0,0)=alphaVector(0)+alphaVector(3);
    Hessian.insert(0,2)=alphaVector(0);
    Hessian.insert(1,1)=alphaVector(2);
    Hessian.insert(2,0)=alphaVector(0);
    Hessian.insert(2,2)=alphaVector(0)+alphaVector(1);
 //   m_hessian.resize(3,3);
    m_hessian=Hessian;
    if (m_QPSolver->isInitialized()) {
        yWarning()<<"[QPslover::setHessianMatrix] The Hessian Matrix should be set just one time! In step adaptation the hessian matrix is constant and just depend on the gains of cost funtion.";
        //        return  false;
    }
    else{
        if (!(m_QPSolver->data()->setHessianMatrix(Hessian))){
            yError()<<"[QPslover::setHessianMatrix]Unable to set first time the hessian matrix.";
            return false;
        };
    }
    return true;
}

bool QPSolver::setGradientVector(const iDynTree::Vector4& alphaVector,const iDynTree::VectorFixSize<5>& nominalValuesVector){

    m_gradient.resize(3,1);
    m_gradient<<(-alphaVector(0)* nominalValuesVector(3)-nominalValuesVector(0)*alphaVector(3)),
            (-alphaVector(2)*nominalValuesVector(1)),
            (-nominalValuesVector(2)*alphaVector(1)-nominalValuesVector(3)*alphaVector(0));

    if(m_QPSolver->isInitialized()){
        if(!m_QPSolver->updateGradient<Eigen::Dynamic>(m_gradient)){
            yError()<<"[QPSolver::setGradientVector]:unable to update the Gradient Vector";
            return false;
        }
    }
    else {
        if(!m_QPSolver->data()->setGradient<Eigen::Dynamic>(m_gradient)){
            yError()<<"[QPSolver::setGradientVector]:unable to set the Gradient Vector for the first time";
            return false;
        }
    }
    return true;
}

Eigen::SparseMatrix<double> QPSolver::evaluateConstraintsMatrix(const iDynTree::Vector3& currentValuesVector){
    Eigen::SparseMatrix<double> constraintMatrix;//the constraint Matrix includes both equality and inequality constraints!
    constraintMatrix.resize(5,3);
    constraintMatrix.reserve(7);

    constraintMatrix.insert(0,0)=1;
    constraintMatrix.insert(0,1)=(currentValuesVector(0)+currentValuesVector(2))-currentValuesVector(1)-((currentValuesVector(2))/2);
    constraintMatrix.insert(0,2)=1;
    constraintMatrix.insert(1,0)=1;
    constraintMatrix.insert(2,0)=-1;
    constraintMatrix.insert(3,1)=1;
    constraintMatrix.insert(4,1)=-1;

    return constraintMatrix;
}


bool QPSolver::setConstraintsMatrix(const iDynTree::Vector3 &currentValuesVector){
    m_constraintsMAtrix.resize(5,3);
    m_constraintsMAtrix.reserve(7);
     m_constraintsMAtrix=evaluateConstraintsMatrix(currentValuesVector);
     Eigen::Matrix<double,5,3> miladtemp=m_constraintsMAtrix;

    if(m_QPSolver->isInitialized()){
        if(!m_QPSolver->updateLinearConstraintsMatrix(m_constraintsMAtrix)){
            yError()<<"[setConstraintsMatrix] unable to update the linear constraints matrix of QPSolver corresponding to step adaptator!";
            return false;
        }
    }
    else{
        if (!m_QPSolver->data()->setLinearConstraintsMatrix(m_constraintsMAtrix)) {
            yError()<<"[setConstraintsMatrix] unable to set the the linear constraints matrix of QPSolver corresponding to step adaptator for the first time ";
            return false;
        }
    }
    return true;
}

bool QPSolver::setBoundsVectorOfConstraints(const iDynTree::VectorFixSize<5> &nominalValuesVector,const iDynTree::Vector3& currentValuesVector,const iDynTree::Vector4& tolerenceOfBounds){

    //    Eigen::Vector5d lowerBounds;
    //  Eigen::VectorFix upperBounds;
    Eigen::Matrix<double,5,1>  upperBounds;
    Eigen::Matrix<double,5,1>  lowerBounds;
    m_lowerBound.resize(5,1);
    m_upperBound.resize(5,1);
    double StepDuration=((log(nominalValuesVector(1)))/nominalValuesVector(4));

    m_upperBound<<currentValuesVector(2)/2+currentValuesVector(0),
            nominalValuesVector(0)+tolerenceOfBounds(0),
            nominalValuesVector(0)-tolerenceOfBounds(1),
            exp((StepDuration+tolerenceOfBounds(2))*nominalValuesVector(4)),
            exp((StepDuration-tolerenceOfBounds(2))*nominalValuesVector(4));

    m_lowerBound<<currentValuesVector(2)/2+currentValuesVector(0),
            - OsqpEigen::INFTY,
            - OsqpEigen::INFTY,
            - OsqpEigen::INFTY,
            - OsqpEigen::INFTY;

    if (m_QPSolver->isInitialized()) {
        if (!m_QPSolver->updateBounds(m_lowerBound,m_upperBound)) {
            yError()<<"[setBoundsVectorOfConstraints]Unable to update the bounds of constraints in QP problem in step adaptation";
            return false;
        }
    }
    else {

        if (!m_QPSolver->data()->setLowerBound(m_lowerBound)) {
            yError()<<"[setBoundsVectorOfConstraints] Unable to set the lower bounds of constraints in QP problem in step adaptation ";
            return false;
        }
        if (!m_QPSolver->data()->setUpperBound(m_upperBound)){
            yError()<<"[setBoundsVectorOfConstraints] Unable to set the  upper bounds of constraints in QP problem in step adaptation";
        return false;
    }

    }


    return true;
}

bool QPSolver::isInitialized()
{
    return m_QPSolver->isInitialized();
}

bool QPSolver::initialize()
{
    return m_QPSolver->initSolver();
}

bool QPSolver::getPrimalVariable(Eigen::VectorXd &primalVariable)
{
    if (!m_QPSolver->isInitialized()) {
        yError() << "[getPrimalVariable in QPSolver] The solver has not initilialized.";
        return false;
    }
    return m_QPSolver->getPrimalVariable(primalVariable);
}

bool QPSolver::setPrimalVariable(const Eigen::VectorXd &primalVariable)
{
    if(!m_QPSolver->isInitialized()){
        yError()<<"[solve in QPSolver.cpp] The solver has not initilialized";
        return false;
    }
    return m_QPSolver->setPrimalVariable(primalVariable);
}
bool QPSolver::solve()
{
    if (!m_QPSolver->isInitialized()) {
        yError()<<"[solve in QPSolver.cpp] The solver has not initilialized";
        return false;
    }
    return m_QPSolver->solve();
}

iDynTree::VectorDynSize QPSolver::getSolution()
{
    Eigen::VectorXd solutionEigen=m_QPSolver->getSolution();
    int solutionSize=m_inputSize;
    iDynTree::VectorDynSize solution(solutionSize);
    iDynTree::toEigen(solution)=solutionEigen;
    return solution;
}

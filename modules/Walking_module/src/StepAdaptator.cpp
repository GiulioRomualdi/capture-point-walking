

// std
#define NOMINMAX
#include <algorithm>

// yarp
#include <yarp/os/LogStream.h>

// iDynTree
#include <iDynTree/Core/EigenSparseHelpers.h>
#include <iDynTree/Core/Direction.h>

#include <StepAdaptator.hpp>
#include <Utils.hpp>

StepAdaptator::StepAdaptator()
    : m_xPositionsBuffer(2)
    , m_yPositionsBuffer(2)
    , m_zPositionsBuffer(3)
    , m_zzPositionsBuffer(2)
    , m_yawsBuffer(2)
    , m_timesBuffer(2)
    , m_zTimesBuffer(3)
    ,m_zzTimesBuffer(2)
    //    , xSpline(2)
    //    , ySpline(2)
    //    , zSpline(3)
    //    , yawSpline(2)
{}

bool StepAdaptator::initialize(const yarp::os::Searchable &config)
{
    // inputs are zmp position, sigma dcm offset
    m_inputSize = 5;

    // constraints are dynamics (2) zmp position (2) impact time(1)
    m_numberOfConstraint = 5;

    m_currentQPSolver = std::make_shared<QPSolver>(m_inputSize, m_numberOfConstraint);


    if(!YarpHelper::getVectorFromSearchable(config, "next_zmp_position_weight",  m_zmpPositionWeight))
    {
        yError() << "[StepAdaptator::initialize] Unable to get the vector";
        return false;
    }

    if(!YarpHelper::getVectorFromSearchable(config, "next_dcm_offset_weight",  m_dcmOffsetWeight))
    {
        yError() << "[StepAdaptator::initialize] Unable to get the vector";
        return false;
    }

    if(!YarpHelper::getNumberFromSearchable(config, "sigma_weight", m_sigmaWeight))
    {
        yError() << "[StepAdaptator::initialize] Unable to get the number";
        return false;
    }

    if(!YarpHelper::getVectorFromSearchable(config, "zmp_position_tolerance",  m_zmpPositionTollerance))
    {
        yError() << "[StepAdaptator::initialize] Unable to get the vector";
        return false;
    }

    if(!YarpHelper::getNumberFromSearchable(config, "step_duration_tolerance", m_stepDurationTolerance))
    {
        yError() << "[StepAdaptator::initialize] Unable to get the number";
        return false;
    }

    if(!m_currentQPSolver->setHessianMatrix(m_zmpPositionWeight, m_dcmOffsetWeight, m_sigmaWeight))
    {
        yError() << "[StepAdaptator::initialize] Unable set the hessian";
        return false;
    }

    // reset the solver
    reset();

    return true;

}

void StepAdaptator::setNominalNextStepPosition(const iDynTree::Vector2& nominalZmpPosition)
{
    m_zmpPositionNominal = nominalZmpPosition;
}

void StepAdaptator::setTimings(const double & omega, const double & currentTime, const double& nextImpactTime,
                               const double &nextDoubleSupportDuration)
{
    m_nextDoubleSupportDuration = nextDoubleSupportDuration;
    m_currentTime = currentTime;

    m_stepTiming = nextImpactTime + nextDoubleSupportDuration / 2 - currentTime;
    m_remainingSingleSupportDuration = nextImpactTime - currentTime;

    m_sigmaNominal = std::exp(omega * m_stepTiming);
    m_omega = omega;
}

void StepAdaptator::setNominalDcmOffset(const iDynTree::Vector2& nominalDcmOffset)
{
    m_dcmOffsetNominal = nominalDcmOffset;
}

void StepAdaptator::setCurrentZmpPosition(const iDynTree::Vector2& currentZmpPosition)
{
    m_currentZmpPosition = currentZmpPosition;
}

void StepAdaptator::setCurrentDcmPosition(const iDynTree::Vector2& currentDcmPosition)
{
    m_currentDcmPosition = currentDcmPosition;
}

bool StepAdaptator::solve()
{
    if (!m_currentQPSolver->setGradientVector(m_zmpPositionWeight, m_dcmOffsetWeight, m_sigmaWeight,
                                              m_zmpPositionNominal, m_dcmOffsetNominal, m_sigmaNominal))
    {
        yError() << "[StepAdaptator::RunStepAdaptator] Unable to set the Gradient Vector";
        return false;
    }

    if(!m_currentQPSolver->setConstraintsMatrix(m_currentDcmPosition, m_currentZmpPosition))
    {
        yError() << "[StepAdaptator::RunStepAdaptator] Unable to set the constraint matrix";
        return false;
    }

    if(!m_currentQPSolver->setBoundsVectorOfConstraints(m_currentZmpPosition, m_zmpPositionNominal, m_zmpPositionTollerance,
                                                        m_stepTiming, m_stepDurationTolerance, m_remainingSingleSupportDuration,
                                                        m_omega))
    {
        yError() << "[StepAdaptator::RunStepAdaptator] Unable to set the bounds";
        return false;
    }


    m_isSolutionEvaluated = false;
    if(!m_currentQPSolver->isInitialized())
    {
        if(!m_currentQPSolver->initialize())
        {
            yError() << "[StepAdaptator::solve] Unable to initialize the QP solver.";
            return false;
        }
    }

    if(!m_currentQPSolver->solve())
    {
        yError() << "[StepAdaptator::solve] Unable to solve the step adaptation problem.";
        return false;
    }

    m_isSolutionEvaluated = true;
    return true;
}

double StepAdaptator::getDesiredImpactTime()
{
    double optimalStepDuration = std::log(m_currentQPSolver->getSolution()(2)) / m_omega;

    return m_currentTime + optimalStepDuration - m_nextDoubleSupportDuration / 2;
}

iDynTree::Vector2 StepAdaptator::getDesiredZmp()
{
    iDynTree::Vector2 desiredZmp;

    desiredZmp(0) = m_currentQPSolver->getSolution()(0);
    desiredZmp(1) = m_currentQPSolver->getSolution()(1);

    return desiredZmp;
}

// bool StepAdaptator::getAdaptatedFootTrajectory(double maxFootHeight,double dt,const iDynTree::VectorFixSize<5>& nominalValues,iDynTree::Transform& adaptatedFootTransform,iDynTree::Twist& adaptedFootTwist, const iDynTree::Transform& currentFootTransform, const iDynTree::Twist& currentFootTwist,const iDynTree::Transform& finalFootTransform,const double& timePassed, const double deltaDS ){
//     //remember you should also add acceleration as output!!!!
//     iDynTree::CubicSpline xSpline, ySpline, zSpline, yawSpline;
//     //yInfo()<<maxFootHeight<<"maxxxxx fooottt height";
//     if (timePassed<=((timePassed+log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS)*0.8)) {
//         //     yInfo()<<"okkkkkk"<<(timePassed+log(m_outputStepAdaptator(1))/nominalValues(4))/2;
//        // yInfo()<<"salam00"<<timePassed<<((timePassed+log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS)/2)<<dt;
//         //yInfo()<<currentFootTwist.getLinearVec3()(2)<<"velocity z";

//         m_zTimesBuffer(0)=0.0;
//         m_zTimesBuffer(1)=(timePassed+log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS)*0.8-timePassed;
//         m_zTimesBuffer(2)=log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS;
//        // yInfo()<<m_zTimesBuffer(0)<<m_zTimesBuffer(1)<<m_zTimesBuffer(2)<<"timeeeeeeeeee00000";
//         m_zPositionsBuffer(0)= currentFootTransform.getPosition()(2);
//         m_zPositionsBuffer(1)=maxFootHeight;
//         m_zPositionsBuffer(2)= finalFootTransform.getPosition()(2);

//         zSpline.setInitialConditions(currentFootTwist.getLinearVec3()(2), 0.0);
//         zSpline.setFinalConditions(0.0,0.0);


//         if (!zSpline.setData(m_zTimesBuffer, m_zPositionsBuffer)){
//             std::cerr << "[StepAdaptator::getAdaptatedFootTrajectory] Failed to initialize the z-dimension spline." << std::endl;
//             return false;
//         }
//     }


//     else{
//         m_zzTimesBuffer(0)=0.0;
//         m_zzTimesBuffer(1)=log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS;
//         iDynTree::Position PositionsBuffer=currentFootTransform.getPosition();
//         m_zzPositionsBuffer(0)=PositionsBuffer(2);
//         m_zzPositionsBuffer(1)= finalFootTransform.getPosition()(2);
//       //  yInfo()<<m_zzTimesBuffer(0)<<m_zzTimesBuffer(1)<<deltaDS<<m_outputStepAdaptator(1)<<"timeeeeeeeeee111111";
//       //  yInfo()<<"salam11"<<timePassed<<((timePassed+log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS)/2)<<PositionsBuffer(2)<<currentFootTwist.getLinearVec3()(2);
//         zSpline.setInitialConditions(currentFootTwist.getLinearVec3()(2), 0.0);
//         zSpline.setFinalConditions(0.0,0.0);

//         if (!zSpline.setData(m_zzTimesBuffer, m_zzPositionsBuffer)){
//             std::cerr << "[StepAdaptator::getAdaptatedFootTrajectory] Failed to initialize the z-dimension spline." << std::endl;
//             return false;
//         }
//     }

//     m_xPositionsBuffer(0)= currentFootTransform.getPosition()(0);
//     m_yPositionsBuffer(0)= currentFootTransform.getPosition()(1);

//     m_yawsBuffer(0)=currentFootTransform.getRotation().asRPY()(2);

//     m_xPositionsBuffer(1)= m_outputStepAdaptator(0)-0.03;
//     m_yPositionsBuffer(1)= finalFootTransform.getPosition()(1);

//     m_yawsBuffer(1)=finalFootTransform.getRotation().asRPY()(2);

//     m_timesBuffer(0) = 0.0;
//     m_timesBuffer(1) = log(m_outputStepAdaptator(1))/nominalValues(4)-deltaDS;

//     double yawAngle;

//     iDynTree::AngularMotionVector3 rightTrivializedAngVelocity;
//     iDynTree::Vector3 rpyDerivativeCurrent;
//     iDynTree::Vector3 rpyDerivative;
//     iDynTree::toEigen(rpyDerivativeCurrent)= iDynTree::toEigen(iDynTree::Rotation::RPYRightTrivializedDerivativeInverse(0.0, 0.0, m_yawsBuffer(0))) * iDynTree::toEigen(currentFootTwist.getAngularVec3());

//     xSpline.setInitialConditions(currentFootTwist.getLinearVec3()(0), 0.0);
//     ySpline.setInitialConditions(currentFootTwist.getLinearVec3()(1), 0.0);
//     yawSpline.setInitialConditions(rpyDerivativeCurrent(2), 0.0);

//     xSpline.setFinalConditions(0.0,0.0);
//     ySpline.setFinalConditions(0.0,0.0);
//     yawSpline.setFinalConditions(0.0, 0.0);



//     if (!xSpline.setData(m_timesBuffer, m_xPositionsBuffer)){
//         std::cerr << "[StepAdaptator::getAdaptatedFootTrajectory] Failed to initialize the x-dimension spline." << std::endl;
//         return false;
//     }
//     if (!ySpline.setData(m_timesBuffer, m_yPositionsBuffer)){
//         std::cerr << "[StepAdaptator::getAdaptatedFootTrajectory] Failed to initialize the y-dimension spline." << std::endl;
//         return false;
//     }

//     if (!yawSpline.setData(m_timesBuffer, m_yawsBuffer)){
//         std::cerr << "[StepAdaptator::getAdaptatedFootTrajectory] Failed to initialize the yaw-dimension spline." << std::endl;
//         return false;
//     }



//     iDynTree::Transform newTransform;
//     iDynTree::Position newPosition;
//     iDynTree::Vector3 linearVelocity;
//     iDynTree::Vector3 linearAcceleration;
//     iDynTree::Vector3 rpySecondDerivative;

//     //adaptatedFootTransform
//     newPosition(0) = xSpline.evaluatePoint(dt, linearVelocity(0), linearAcceleration(0));
//     newPosition(1) = ySpline.evaluatePoint(dt, linearVelocity(1), linearAcceleration(1));
//     newPosition(2) = zSpline.evaluatePoint(dt, linearVelocity(2), linearAcceleration(2));

//     yawAngle = yawSpline.evaluatePoint(dt, rpyDerivative(2), rpySecondDerivative(2));

//     adaptatedFootTransform.setPosition(newPosition);
//     adaptatedFootTransform.setRotation(iDynTree::Rotation::RPY(0.0, 0.0, yawAngle));

//     rpyDerivative(0)=0;
//     rpyDerivative(1)=0;

//     iDynTree::toEigen(rightTrivializedAngVelocity) = iDynTree::toEigen(iDynTree::Rotation::RPYRightTrivializedDerivative(0.0, 0.0, yawAngle)) *iDynTree::toEigen(rpyDerivative);
//     adaptedFootTwist.setLinearVec3(linearVelocity);
//     adaptedFootTwist.setAngularVec3(rightTrivializedAngVelocity);


//     return true;
// }


void StepAdaptator::reset(){
    m_isSolutionEvaluated = false;
}

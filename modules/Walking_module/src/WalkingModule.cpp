/**
 * @file WalkingModule.cpp
 * @authors Giulio Romualdi <giulio.romualdi@iit.it>
 * @copyright 2018 iCub Facility - Istituto Italiano di Tecnologia
 *            Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 * @date 2018
 */

// std
#include <iostream>
#include <memory>

// YARP
#include <yarp/os/RFModule.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Vector.h>
#include <yarp/os/LogStream.h>

// iDynTree
#include <iDynTree/Core/VectorFixSize.h>
#include <iDynTree/Core/EigenHelpers.h>
#include <iDynTree/yarp/YARPConversions.h>
#include <iDynTree/yarp/YARPEigenConversions.h>
#include <iDynTree/Model/Model.h>

#include <WalkingModule.hpp>
#include <Utils.hpp>

void WalkingModule::propagateTime()
{
    // propagate time
    m_time += m_dT;
}

bool WalkingModule::advanceReferenceSignals()
{
    // check if vector is not initialized
    if(m_leftTrajectory.empty()
            || m_rightTrajectory.empty()
            || m_leftInContact.empty()
            || m_rightInContact.empty()
            || m_DCMPositionDesired.empty()
            || m_DCMVelocityDesired.empty()
            || m_comHeightTrajectory.empty())
    {
        yError() << "[advanceReferenceSignals] Cannot advance empty reference signals.";
        return false;
    }

    m_rightTrajectory.pop_front();
    m_rightTrajectory.push_back(m_rightTrajectory.back());

    m_leftTrajectory.pop_front();
    m_leftTrajectory.push_back(m_leftTrajectory.back());

    m_rightTwistTrajectory.pop_front();
    m_rightTwistTrajectory.push_back(m_rightTwistTrajectory.back());

    m_leftTwistTrajectory.pop_front();
    m_leftTwistTrajectory.push_back(m_leftTwistTrajectory.back());

    m_rightInContact.pop_front();
    m_rightInContact.push_back(m_rightInContact.back());

    m_leftInContact.pop_front();
    m_leftInContact.push_back(m_leftInContact.back());

    m_isLeftFixedFrame.pop_front();
    m_isLeftFixedFrame.push_back(m_isLeftFixedFrame.back());

    m_DCMPositionDesired.pop_front();
    m_DCMPositionDesired.push_back(m_DCMPositionDesired.back());

    m_DCMVelocityDesired.pop_front();
    m_DCMVelocityDesired.push_back(m_DCMVelocityDesired.back());

    m_comHeightTrajectory.pop_front();
    m_comHeightTrajectory.push_back(m_comHeightTrajectory.back());

    m_comHeightVelocity.pop_front();
    m_comHeightVelocity.push_back(m_comHeightVelocity.back());

    // at each sampling time the merge points are decreased by one.
    // If the first merge point is equal to 0 it will be dropped.
    // A new trajectory will be merged at the first merge point or if the deque is empty
    // as soon as possible.
    if(!m_mergePoints.empty())
    {
        for(auto& mergePoint : m_mergePoints)
            mergePoint--;

        if(m_mergePoints[0] == 0)
            m_mergePoints.pop_front();
    }
    return true;
}

double WalkingModule::getPeriod()
{
    //  period of the module (seconds)
    return m_dT;
}

bool WalkingModule::setRobotModel(const yarp::os::Searchable& rf)
{
    // load the model in iDynTree::KinDynComputations
    std::string model = rf.check("model",yarp::os::Value("model.urdf")).asString();
    std::string pathToModel = yarp::os::ResourceFinder::getResourceFinderSingleton().findFileByName(model);

    yInfo() << "The model is found in: " << pathToModel;

    // only the controlled joints are extracted from the URDF file
    if(!m_loader.loadReducedModelFromFile(pathToModel, m_robotControlHelper->getAxesList()))
    {
        yError() << "[setRobotModel] Error while loading the model from " << pathToModel;
        return false;
    }
    return true;
}

bool WalkingModule::configure(yarp::os::ResourceFinder& rf)
{
    // module name (used as prefix for opened ports)
    m_useStepAdaptation = rf.check("use_step_adaptation", yarp::os::Value(false)).asBool();
    m_useMPC = rf.check("use_mpc", yarp::os::Value(false)).asBool();
    m_useQPIK = rf.check("use_QP-IK", yarp::os::Value(false)).asBool();
    m_useOSQP = rf.check("use_osqp", yarp::os::Value(false)).asBool();
    m_dumpData = rf.check("dump_data", yarp::os::Value(false)).asBool();

    yarp::os::Bottle& generalOptions = rf.findGroup("GENERAL");
    m_dT = generalOptions.check("sampling_time", yarp::os::Value(0.016)).asDouble();
    std::string name;
    if(!YarpHelper::getStringFromSearchable(generalOptions, "name", name))
    {
        yError() << "[configure] Unable to get the string from searchable.";
        return false;
    }
    setName(name.c_str());

    m_robotControlHelper = std::make_unique<RobotHelper>();
    yarp::os::Bottle& robotControlHelperOptions = rf.findGroup("ROBOT_CONTROL");
    robotControlHelperOptions.append(generalOptions);
    if(!m_robotControlHelper->configureRobot(robotControlHelperOptions))
    {
        yError() << "[configure] Unable to configure the robot.";
        return false;
    }

    yarp::os::Bottle& forceTorqueSensorsOptions = rf.findGroup("FT_SENSORS");
    forceTorqueSensorsOptions.append(generalOptions);
    if(!m_robotControlHelper->configureForceTorqueSensors(forceTorqueSensorsOptions))
    {
        yError() << "[configure] Unable to configure the Force Torque sensors.";
        return false;
    }

    if(!setRobotModel(rf))
    {
        yError() << "[configure] Unable to set the robot model.";
        return false;
    }

    // open RPC port for external command
    std::string rpcPortName = "/" + getName() + "/rpc";
    this->yarp().attachAsServer(this->m_rpcPort);
    if(!m_rpcPort.open(rpcPortName))
    {
        yError() << "[configure] Could not open" << rpcPortName << " RPC port.";
        return false;
    }

    std::string desiredUnyciclePositionPortName = "/" + getName() + "/goal:i";
    if(!m_desiredUnyciclePositionPort.open(desiredUnyciclePositionPortName))
    {
        yError() << "[configure] Could not open" << desiredUnyciclePositionPortName << " port.";
        return false;
    }


    //reading some data for zmp saturation function from configuration file
    m_useZMPSaturation = rf.check("use_zmp_saturation", yarp::os::Value("False")).asBool();

    if(!YarpHelper::getNumberFromSearchable(rf, "epsilon", epsilonZMP))
    {
        yError() << "[configure] Unable get epsilon(double) from searchable.";
        return false;
    }

    if(!YarpHelper::getNumberFromSearchable(rf, "zmp_saturation_threshold", thresholdFz))
    {
        yError() << "[configure] Unable get thresholdFz(double) from searchable.";
        return false;
    }



    //low pass filter on zmp -- preparing filter
    m_useZMPFilter = rf.check("use_zmp_filter", yarp::os::Value("False")).asBool();
    if(m_useZMPFilter)
    {
        double cutFrequency;
        if(!YarpHelper::getNumberFromSearchable(rf, "zmp_cut_frequency", cutFrequency))
        {
            yError() << "[configure] Unable get double from searchable.";
            return false;
        }

        m_ZMPFilter = std::make_unique<iCub::ctrl::FirstOrderLowPassFilter>(cutFrequency,m_dT);
    }


    // initialize the trajectory planner
    m_trajectoryGenerator = std::make_unique<TrajectoryGenerator>();
    yarp::os::Bottle& trajectoryPlannerOptions = rf.findGroup("TRAJECTORY_PLANNER");
    trajectoryPlannerOptions.append(generalOptions);
    if(!m_trajectoryGenerator->initialize(trajectoryPlannerOptions))
    {
        yError() << "[configure] Unable to initialize the planner.";
        return false;
    }

    if(m_useStepAdaptation)
    {
        // initialize the step adaptation
        m_stepAdaptator = std::make_unique<StepAdaptator>();
        yarp::os::Bottle& stepAdaptatorOptions = rf.findGroup("STEP_ADAPTATOR");
        stepAdaptatorOptions.append(generalOptions);
        if(!m_stepAdaptator->initialize(stepAdaptatorOptions))
        {
            yError() << "[configure] Unable to initialize the step adaptator!";
            return false;
        }
    }


    if(m_useMPC)
    {
        // initialize the MPC controller
        m_walkingController = std::make_unique<WalkingController>();
        yarp::os::Bottle& dcmControllerOptions = rf.findGroup("DCM_MPC_CONTROLLER");
        dcmControllerOptions.append(generalOptions);
        if(!m_walkingController->initialize(dcmControllerOptions))
        {
            yError() << "[configure] Unable to initialize the controller.";
            return false;
        }
    }
    else
    {
        // initialize the MPC controller
        m_walkingDCMReactiveController = std::make_unique<WalkingDCMReactiveController>();
        yarp::os::Bottle& dcmControllerOptions = rf.findGroup("DCM_REACTIVE_CONTROLLER");
        dcmControllerOptions.append(generalOptions);
        if(!m_walkingDCMReactiveController->initialize(dcmControllerOptions))
        {
            yError() << "[configure] Unable to initialize the controller.";
            return false;
        }
    }

    // initialize the ZMP controller
    m_walkingZMPController = std::make_unique<WalkingZMPController>();
    yarp::os::Bottle& zmpControllerOptions = rf.findGroup("ZMP_CONTROLLER");
    zmpControllerOptions.append(generalOptions);
    if(!m_walkingZMPController->initialize(zmpControllerOptions))
    {
        yError() << "[configure] Unable to initialize the ZMP controller.";
        return false;
    }

    // initialize the inverse kinematics solver
    m_IKSolver = std::make_unique<WalkingIK>();
    yarp::os::Bottle& inverseKinematicsSolverOptions = rf.findGroup("INVERSE_KINEMATICS_SOLVER");
    if(!m_IKSolver->initialize(inverseKinematicsSolverOptions, m_loader.model(),
                               m_robotControlHelper->getAxesList()))
    {
        yError() << "[configure] Failed to configure the ik solver";
        return false;
    }

    if(m_useQPIK)
    {
        yarp::os::Bottle& inverseKinematicsQPSolverOptions = rf.findGroup("INVERSE_KINEMATICS_QP_SOLVER");

        iDynTree::VectorDynSize negativeJointVelocityLimits(m_robotControlHelper->getActuatedDoFs());
        iDynTree::toEigen(negativeJointVelocityLimits) = -iDynTree::toEigen(m_robotControlHelper->getVelocityLimits());
        m_QPIKSolver_osqp = std::make_shared<WalkingQPIK_osqp>();
        if(!m_QPIKSolver_osqp->initialize(inverseKinematicsQPSolverOptions,
                                          m_robotControlHelper->getActuatedDoFs(),
                                          negativeJointVelocityLimits,
                                          m_robotControlHelper->getVelocityLimits()))
        {
            yError() << "[configure] Failed to configure the QP-IK solver (osqp)";
            return false;
        }

        m_QPIKSolver_qpOASES = std::make_shared<WalkingQPIK_qpOASES>();
        if(!m_QPIKSolver_qpOASES->initialize(inverseKinematicsQPSolverOptions,
                                             m_robotControlHelper->getActuatedDoFs(),
                                             negativeJointVelocityLimits,
                                             m_robotControlHelper->getVelocityLimits()))
        {
            yError() << "[configure] Failed to configure the QP-IK solver (qpOASES)";
            return false;
        }
    }

    // initialize the forward kinematics solver
    m_FKSolver = std::make_unique<WalkingFK>();
    yarp::os::Bottle& forwardKinematicsSolverOptions = rf.findGroup("FORWARD_KINEMATICS_SOLVER");
    forwardKinematicsSolverOptions.append(generalOptions);
    if(!m_FKSolver->initialize(forwardKinematicsSolverOptions, m_loader.model()))
    {
        yError() << "[configure] Failed to configure the fk solver";
        return false;
    }

    // initialize the linear inverted pendulum model
    m_stableDCMModel = std::make_unique<StableDCMModel>();
    if(!m_stableDCMModel->initialize(generalOptions))
    {
        yError() << "[configure] Failed to configure the lipm.";
        return false;
    }

    // set PIDs gains
    yarp::os::Bottle& pidOptions = rf.findGroup("PID");
    if (!m_robotControlHelper->configurePIDHandler(pidOptions))
    {
        yError() << "[configure] Failed to configure the PIDs.";
        return false;
    }

    // initialize the logger
    if(m_dumpData)
    {
        m_walkingLogger = std::make_unique<WalkingLogger>();
        yarp::os::Bottle& loggerOptions = rf.findGroup("WALKING_LOGGER");
        if(!m_walkingLogger->configure(loggerOptions, getName()))
        {
            yError() << "[configure] Unable to configure the logger.";
            return false;
        }
    }

    // time profiler
    m_profiler = std::make_unique<TimeProfiler>();
    m_profiler->setPeriod(round(0.1 / m_dT));
    if(m_useMPC)
        m_profiler->addTimer("MPC");

    m_profiler->addTimer("IK");
    m_profiler->addTimer("Total");

    // initialize some variables
    m_firstStep = false;
    m_newTrajectoryRequired = false;
    m_newTrajectoryMergeCounter = -1;
    m_robotState = WalkingFSM::Configured;

    m_inertial_R_worldFrame = iDynTree::Rotation::Identity();

    // resize variables
    m_qDesired.resize(m_robotControlHelper->getActuatedDoFs());
    m_dqDesired.resize(m_robotControlHelper->getActuatedDoFs());

    yInfo() << "[configure] Ready to play!";

    return true;
}




void WalkingModule::reset()
{
    if(m_useMPC)
        m_walkingController->reset();

    if(m_useStepAdaptation)
        m_stepAdaptator->reset();

    m_trajectoryGenerator->reset();

    if(m_dumpData)
        m_walkingLogger->quit();
}

bool WalkingModule::close()
{
    if(m_dumpData)
        m_walkingLogger->quit();

    // restore PID
    m_robotControlHelper->getPIDHandler().restorePIDs();

    // close the ports
    m_rpcPort.close();
    m_desiredUnyciclePositionPort.close();

    // close the connection with robot
    if(!m_robotControlHelper->close())
    {
        yError() << "[close] Unable to close the connection with the robot.";
        return false;
    }

    // clear all the pointer
    m_trajectoryGenerator.reset(nullptr);
    m_walkingController.reset(nullptr);
    m_stepAdaptator.reset(nullptr);
    m_walkingZMPController.reset(nullptr);
    m_IKSolver.reset(nullptr);
    m_QPIKSolver_osqp = nullptr;
    m_QPIKSolver_qpOASES = nullptr;
    m_FKSolver.reset(nullptr);
    m_stableDCMModel.reset(nullptr);

    return true;
}

bool WalkingModule::solveQPIK(const std::shared_ptr<WalkingQPIK> solver, const iDynTree::Position& desiredCoMPosition,
                              const iDynTree::Vector3& desiredCoMVelocity,
                              const iDynTree::Position& actualCoMPosition,
                              const iDynTree::Rotation& desiredNeckOrientation,
                              iDynTree::VectorDynSize &output)
{
    if(!solver->setRobotState(m_robotControlHelper->getJointPosition(),
                              m_FKSolver->getLeftFootToWorldTransform(),
                              m_FKSolver->getRightFootToWorldTransform(),
                              m_FKSolver->getNeckOrientation(),
                              actualCoMPosition))
    {
        yError() << "[solveQPIK] Unable to update the QP-IK solver";
        return false;
    }

    solver->setDesiredNeckOrientation(desiredNeckOrientation.inverse());

    solver->setDesiredFeetTransformation(m_leftTrajectory.front(),
                                         m_rightTrajectory.front());

    solver->setDesiredFeetTwist(m_leftTwistTrajectory.front(),
                                m_rightTwistTrajectory.front());

    solver->setDesiredCoMVelocity(desiredCoMVelocity);

    solver->setDesiredCoMPosition(desiredCoMPosition);

    // set jacobians
    iDynTree::MatrixDynSize jacobian, comJacobian;
    jacobian.resize(6, m_robotControlHelper->getActuatedDoFs() + 6);
    comJacobian.resize(3, m_robotControlHelper->getActuatedDoFs() + 6);

    m_FKSolver->getLeftFootJacobian(jacobian);
    solver->setLeftFootJacobian(jacobian);

    m_FKSolver->getRightFootJacobian(jacobian);
    solver->setRightFootJacobian(jacobian);

    m_FKSolver->getNeckJacobian(jacobian);
    solver->setNeckJacobian(jacobian);

    m_FKSolver->getCoMJacobian(comJacobian);
    solver->setCoMJacobian(comJacobian);

    if(!solver->solve())
    {
        yError() << "[solveQPIK] Unable to solve the QP-IK problem.";
        return false;
    }

    if(!solver->getSolution(output))
    {
        yError() << "[solveQPIK] Unable to get the QP-IK problem solution.";
        return false;
    }

    return true;
}

bool WalkingModule::updateModule()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if(m_robotState == WalkingFSM::Preparing)
    {
        bool motionDone = false;
        if(!m_robotControlHelper->checkMotionDone(motionDone))
        {
            yError() << "[updateModule] Unable to check if the motion is done";
            yInfo() << "[updateModule] Try to prepare again";
            reset();
            m_robotState = WalkingFSM::Stopped;
            return true;
        }
        if(motionDone)
        {
            if(!m_robotControlHelper->switchToControlMode(VOCAB_CM_POSITION_DIRECT))
            {
                yError() << "[updateModule] Failed in setting POSITION DIRECT mode.";
                yInfo() << "[updateModule] Try to prepare again";
                reset();
                m_robotState = WalkingFSM::Stopped;
                return true;
            }

            // send the reference again in order to reduce error
            if(!m_robotControlHelper->setDirectPositionReferences(m_qDesired))
            {
                yError() << "[prepareRobot] Error while setting the initial position using "
                         << "POSITION DIRECT mode.";
                yInfo() << "[updateModule] Try to prepare again";
                reset();
                m_robotState = WalkingFSM::Stopped;
                return true;
            }

            yarp::sig::Vector buffer(m_qDesired.size());
            iDynTree::toYarp(m_qDesired, buffer);
            // instantiate Integrator object
            m_velocityIntegral = std::make_unique<iCub::ctrl::Integrator>(m_dT, buffer);

            // reset the models
            m_walkingZMPController->reset(m_DCMPositionDesired.front());
            m_stableDCMModel->reset(m_DCMPositionDesired.front());

            m_robotState = WalkingFSM::Prepared;
            indexmilad=0;
            yInfo() << "[updateModule] The robot is prepared.";
        }
    }
    else if(m_robotState == WalkingFSM::Walking)
    {
        //        std::vector<StepPhase> jleftFootPhases;
        //        std::vector<StepPhase> jRightFootPhases;
        //        m_trajectoryGenerator->getStepPhases(jleftFootPhases,jRightFootPhases);
        // yInfo()  <<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad]);

        //        std::shared_ptr<FootPrint> jleftFootprints= m_trajectoryGenerator->getLeftFootprint();
        //        std::shared_ptr<FootPrint> jrightFootprints= m_trajectoryGenerator->getRightFootprint();
        //        StepList jLeftstepList=jleftFootprints->getSteps();
        //        StepList jRightstepList=jrightFootprints->getSteps();
        //        Step jmil;
        //        Step jmil10;
        //                if ((jleftFootprints->numberOfSteps())==1) {
        //                    jmil=jLeftstepList.at(0);
        //                   // yInfo()<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime;
        //                }else {
        //                    jmil=jLeftstepList.at(0);
        //                    Step jmil1=jLeftstepList.at(1);
        //                    Step jmil2=jLeftstepList.at(2);
        //                    Step jmil3=jLeftstepList.at(3);
        //                    Step jmil4=jLeftstepList.at(4);
        //                    Step jmil5=jLeftstepList.at(5);


        //                    jmil10=jRightstepList.at(0);
        //                    Step jmil11=jRightstepList.at(1);
        //                    Step jmil22=jRightstepList.at(2);
        //                    Step jmil33=jRightstepList.at(3);
        //                    Step jmil44=jRightstepList.at(4);
        //                    Step jmil55=jRightstepList.at(5);


        ////                    yInfo()<<"left"<<jmil.impactTime<<jmil.position(0)<<jmil.position(1);
        ////                    yInfo()<<"left"<<jmil1.impactTime<<jmil1.position(0)<<jmil1.position(1);
        ////                    yInfo()<<"left"<<jmil2.impactTime<<jmil2.position(0)<<jmil2.position(1);
        ////                    yInfo()<<"left"<<jmil3.impactTime<<jmil3.position(0)<<jmil3.position(1);
        ////                    yInfo()<<"left"<<jmil4.impactTime<<jmil4.position(0)<<jmil4.position(1);
        ////                    yInfo()<<"left"<<jmil5.impactTime<<jmil5.position(0)<<jmil5.position(1);


        ////                    yInfo()<<"right"<<jmil10.impactTime<<jmil10.position(0)<<jmil10.position(1);
        ////                    yInfo()<<"right"<<jmil11.impactTime<<jmil11.position(0)<<jmil11.position(1);
        ////                    yInfo()<<"right"<<jmil22.impactTime<<jmil22.position(0)<<jmil22.position(1);
        ////                    yInfo()<<"right"<<jmil33.impactTime<<jmil33.position(0)<<jmil33.position(1);
        ////                    yInfo()<<"right"<<jmil44.impactTime<<jmil44.position(0)<<jmil44.position(1);
        ////                    yInfo()<<"right"<<jmil55.impactTime<<jmil55.position(0)<<jmil55.position(1);

        //                }
        //             Step jmil1=jLeftstepList.at(0);
        //        int numberOfStep =jleftFootprints->numberOfSteps();

             indexmilad=indexmilad+1;                   //yInfo()  <<static_cast<int>(jleftFootPhases[2])<<static_cast<int>(jleftFootPhases[2])<<static_cast<int>(jleftFootPhases[2])<<static_cast<int>(jleftFootPhases[2])<<static_cast<int>(jleftFootPhases[2]);
        //   yInfo()<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime;
        //    yInfo()<<jmil1.impactTime<<jmil1.impactTime<<jmil1.impactTime<<jmil1.impactTime<<jmil1.impactTime<<jmil1.impactTime<<jmil1.impactTime;
        //        yInfo()<<numberOfStep<<numberOfStep;
        //yInfo() << "milad milad miladdddddddddddddddddddddddddddddddddddddddddddddddddddddd: " <<  jleftFootPhases[1]::"Stance";













        iDynTree::Vector2 measuredDCM, measuredZMP;
        iDynTree::Position measuredCoM;
        iDynTree::Vector3 measuredCoMVelocity;

        bool resetTrajectory = false;

        m_profiler->setInitTime("Total");

        // check desired planner input
        yarp::sig::Vector* desiredUnicyclePosition = nullptr;
        desiredUnicyclePosition = m_desiredUnyciclePositionPort.read(false);
        if(desiredUnicyclePosition != nullptr)
            if(!setPlannerInput((*desiredUnicyclePosition)(0), (*desiredUnicyclePosition)(1)))
            {
                yError() << "[updateModule] Unable to set the planner input";
                return false;
            }


        if (m_mergePoints.front() == 21 && desiredUnicyclePosition == nullptr) {

            if(!setPlannerInput(m_desiredPosition(0) ,m_desiredPosition(1)))
            {
                yError() << "[updateModule] Unable to recall the setplannerInput (when terminal (SetGoal) instead of JoyStick is used)";
                return false;
            }
        }


        // if a new trajectory is required check if its the time to evaluate the new trajectory or
        // the time to attach new one
        if(m_newTrajectoryRequired)
        {
            // when we are near to the merge point the new trajectory is evaluated
            if(m_newTrajectoryMergeCounter == 20)
            {

                double initTimeTrajectory;
                initTimeTrajectory = m_time + m_newTrajectoryMergeCounter * m_dT;

                iDynTree::Transform measuredTransform = m_isLeftFixedFrame.front() ?
                            m_rightTrajectory[m_newTrajectoryMergeCounter] :
                            m_leftTrajectory[m_newTrajectoryMergeCounter];

                // ask for a new trajectory
                if(!askNewTrajectories(initTimeTrajectory, !m_isLeftFixedFrame.front(),
                                       measuredTransform, m_newTrajectoryMergeCounter,
                                       m_desiredPosition))
                {
                    yError() << "[updateModule] Unable to ask for a new trajectory.";
                    return false;
                }
            }

            if(m_newTrajectoryMergeCounter == 2)
            {
                if(!updateTrajectories(m_newTrajectoryMergeCounter))
                {
                    yError() << "[updateModule] Error while updating trajectories. They were not computed yet.";
                    return false;
                }
                m_newTrajectoryRequired = false;
                resetTrajectory = true;
            }

            m_newTrajectoryMergeCounter--;
            indexmilad=0;
        }

        if (m_robotControlHelper->getPIDHandler().usingGainScheduling())
        {
            if (!m_robotControlHelper->getPIDHandler().updatePhases(m_leftInContact, m_rightInContact, m_time))
            {
                yError() << "[updateModule] Unable to get the update PID.";
                return false;
            }
        }

        // get feedbacks and evaluate useful quantities
        if(!m_robotControlHelper->getFeedbacks(100))
        {
            yError() << "[updateModule] Unable to get the feedback.";
            return false;
        }

        if(!updateFKSolver())
        {
            yError() << "[updateModule] Unable to update the FK solver.";
            return false;
        }

        if(!evaluateCoM(measuredCoM, measuredCoMVelocity))
        {
            yError() << "[updateModule] Unable to evaluate the CoM.";
            return false;
        }


        if(!evaluateDCM(measuredDCM))
        {
            yError() << "[updateModule] Unable to evaluate the DCM.";
            return false;
        }

        if(!evaluateZMP(measuredZMP))
        {
            yError() << "[updateModule] Unable to evaluate the ZMP.";
            return false;
        }
        //                    yInfo()<<"xxxxxxxzzzzmmmppppp-------"<<measuredZMP(0);
        //                    yInfo()<<"yyyyyyyzzzzmmmppppp-------"<<measuredZMP(1);
        //yInfo()<<
        iDynTree::Vector2 mildds= m_DCMPositionDesired.at((m_mergePoints.front()));
     //   yInfo()<<"xxxxxxxDDDDDCCCCMMMMM-------"<<mildds(0);
        //m_mergePoints
        //                    yInfo()<<"xxxxxxxDDDDDCCCCMMMMM-------"<<measuredDCM(0);
        //                    yInfo()<<"yyyyyyyDDDDDCCCCMMMMM-------"<<measuredDCM(1);

        if(m_useZMPFilter)
        {
            // filter the zmp
            yarp::sig::Vector tempZMPFilter;
            iDynTree::Vector2 tempMeasuredZMP;
            iDynTree::toYarp(measuredZMP,tempZMPFilter);
            tempZMPFilter = m_ZMPFilter->filt(tempZMPFilter);
            iDynTree::toiDynTree(tempZMPFilter,measuredZMP);
        }


        // evaluate 3D-LIPM reference signal
        m_stableDCMModel->setInput(m_DCMPositionDesired.front());
        if(!m_stableDCMModel->integrateModel())
        {
            yError() << "[updateModule] Unable to propagate the 3D-LIPM.";
            return false;
        }

        iDynTree::Vector2 desiredCoMPositionXY;
        if(!m_stableDCMModel->getCoMPosition(desiredCoMPositionXY))
        {
            yError() << "[updateModule] Unable to get the desired CoM position.";
            return false;
        }

        iDynTree::Vector2 desiredCoMVelocityXY;
        if(!m_stableDCMModel->getCoMVelocity(desiredCoMVelocityXY))
        {
            yError() << "[updateModule] Unable to get the desired CoM velocity.";
            return false;
        }



            double switchOverSwingRatio;
            double comHeight;
            double stepTiming;
            double sigma;
            double nextStepPosition;
            double stepLength;
            double nominalDCMOffset;
            double omega;
            iDynTree::VectorFixSize<5> nominalValues;
            iDynTree::Vector3 currentValues;
        if(!m_trajectoryGenerator->getNominalCoMHeight(comHeight)){
            yError() << "[updateModule] Unable to get the nominal CoM height!";
            return false;
        }

        if(!m_trajectoryGenerator->getSwitchOverSwingRatio(switchOverSwingRatio)){
            yError() << "[updateModule] Unable to get the ratio of double support to single support!";
            return false;
        }
omega=sqrt(9.81/comHeight);
            nominalValues(4)=omega;
        std::shared_ptr<FootPrint> jleftFootprints= m_trajectoryGenerator->getLeftFootprint();
        std::shared_ptr<FootPrint> jrightFootprints= m_trajectoryGenerator->getRightFootprint();
        StepList jLeftstepList=jleftFootprints->getSteps();
        StepList jRightstepList=jrightFootprints->getSteps();
        Step jmil;
        Step jmil10;
        //                if ((jleftFootprints->numberOfSteps())==1) {
        //                    jmil=jLeftstepList.at(0);
        //                   // yInfo()<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime<<jmil.impactTime;
        //                }else {
        //                    jmil=jLeftstepList.at(0);
        //                    Step jmil1=jLeftstepList.at(1);

        std::vector<StepPhase> jleftFootPhases;
        std::vector<StepPhase> jRightFootPhases;
        m_trajectoryGenerator->getStepPhases(jleftFootPhases,jRightFootPhases);
                    iDynTree::Vector6 leftAdaptedStepParameters;
//        yInfo()<<"asgharrrrrrrrrrrrrrrrrrrr"<<"aaaaaaaaaaaaa"<<jRightFootPhases.size();
//        yInfo()<<"gggggggggggggggggggggggg"<<"gggggggggggg"<<indexmilad;
 //       yInfo()  <<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad])<<static_cast<int>(jRightFootPhases[indexmilad]);

       //yInfo()<<m_mergePoints.size()<<jLeftstepList.size()<<jRightstepList.size();
        if (2==static_cast<int>(jRightFootPhases[indexmilad]) && jRightstepList.size()>1) {
//jRightstepList.at(1).impactTime;
//            yInfo()<<"asgharrrrrrrrrrrrrrrrrrrr";
//yInfo()<<"milllllllllllllllllllllaaaaaa"<<jLeftstepList.at(0).impactTime<<jLeftstepList.at(0).impactTime<<jLeftstepList.at(0).impactTime<<jLeftstepList.at(0).impactTime<<jLeftstepList.at(0).impactTime;
//            yInfo()<<"akbarrrrrrrrrrrrrrrrrrrr";

//            yInfo()<<"asgharrrrrrrrrrrrrrrrrrrr";
//yInfo()<<"milllllllllllllllllllllaaaaaa"<<jRightstepList.at(1).impactTime<<jRightstepList.at(1).impactTime<<jRightstepList.at(1).impactTime<<jRightstepList.at(1).impactTime<<jRightstepList.at(1).impactTime;
 //          yInfo()<<"akbarrrrrrrrrrrrrrrrrrrr";
stepTiming=(jRightstepList.at(1).impactTime-jLeftstepList.at(0).impactTime)/(1+switchOverSwingRatio);
//switchOverSwingRatio
            sigma=exp(omega*stepTiming);
             nextStepPosition=jRightstepList.at(1).position(0);
             stepLength=(jRightstepList.at(1).position(0)-jLeftstepList.at(0).position(0));
             nominalDCMOffset=stepLength/(exp(omega*stepTiming)-1);
            // Step Adaptator
   //         iDynTree::Vector6 adaptedStepParameters;
            currentValues(0)=measuredZMP(0);
            currentValues(1)=measuredDCM(0);
            currentValues(2)=0;

            nominalValues(0)=nextStepPosition;
            nominalValues(1)=sigma;
            nominalValues(3)=m_DCMPositionDesired[m_mergePoints.front()](0);
            nominalValues(2)=nominalDCMOffset;

            if(m_useStepAdaptation)
            {


                //            m_profiler->setInitTime("MPC");

                if(!m_stepAdaptator->RunStepAdaptator(nominalValues,currentValues))
                {
                    yError() << "[updateModule] Unable to solve the QP problem of step adaptation.";
                    return false;
                }


                if(!m_stepAdaptator->solve())
                {
                    yError() << "[updateModule] Unable to solve the QP problem of step adaptation.";
                    return false;
                }

                if(!m_stepAdaptator->getControllerOutput(leftAdaptedStepParameters))
                {
                    yError() << "[updateModule] Unable to get the step adaptation output.";
                    return false;
                }
        yInfo()<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<"millasjjdhsjjs";
        yInfo()<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<"millasjjdhsjjs";
        yInfo()<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<leftAdaptedStepParameters(1)<<"millasjjdhsjjs";

//        yInfo()<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<"millasjjdhsjjs";
//        yInfo()<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<"millasjjdhsjjs";
//        yInfo()<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<"millasjjdhsjjs";
//        yInfo()<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<"millasjjdhsjjs";
//        yInfo()<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<leftAdaptedStepParameters(0)<<"millasjjdhsjjs";
       //           m_profiler->setEndTime("MPC");///////////////////////////////////////////////////////////////////
            }
        }
        else{
       //     yInfo()<<"this is not right SS";
        }
            iDynTree::Vector6 rightAdaptedStepParameters;

        if (false && jLeftstepList.size()>1) {
//2==static_cast<int>(jRightFootPhases[indexmilad])
            stepTiming=(jLeftstepList.at(1).impactTime-jRightstepList.at(0).impactTime)/(1+switchOverSwingRatio);
            sigma=exp(omega*stepTiming);
            nextStepPosition=jLeftstepList.at(1).position(0);
            stepLength=(jLeftstepList.at(1).position(0)-jRightstepList.at(0).position(0));
           nominalDCMOffset=stepLength/(exp(omega*stepTiming)-1);
//            // Step Adaptator
yInfo()<<"miladddddddddddddddddddddddddd";
            currentValues(0)=measuredZMP(0);
            currentValues(1)=measuredDCM(0);
            currentValues(2)=0;

            nominalValues(0)=nextStepPosition;
            nominalValues(1)=sigma;
            nominalValues(2)=nominalDCMOffset;
            nominalValues(3)=m_DCMPositionDesired[m_mergePoints.front()](0);

            if(m_useStepAdaptation)
            {

                //            m_profiler->setInitTime("MPC");

                if(!m_stepAdaptator->RunStepAdaptator(nominalValues,currentValues))
                {
                    yError() << "[updateModule] Unable to solve the QP problem of step adaptation.";
                    return false;
                }


                if(!m_stepAdaptator->solve())
                {
                    yError() << "[updateModule] Unable to solve the QP problem of step adaptation.";
                    return false;
                }

                if(!m_stepAdaptator->getControllerOutput(rightAdaptedStepParameters))
                {
                    yError() << "[updateModule] Unable to get the step adaptation output.";
                    return false;
                }

                //           m_profiler->setEndTime("MPC");///////////////////////////////////////////////////////////////////
            }
        }
        else{
        //    yInfo()<<"this is not left SS";
        }





        // DCM controller
        iDynTree::Vector2 desiredZMP;
        if(m_useMPC)
        {
            // Model predictive controller
            m_profiler->setInitTime("MPC");
            if(!m_walkingController->setConvexHullConstraint(m_leftTrajectory, m_rightTrajectory,
                                                             m_leftInContact, m_rightInContact))
            {
                yError() << "[updateModule] unable to evaluate the convex hull.";
                return false;
            }

            if(!m_walkingController->setFeedback(measuredDCM))
            {
                yError() << "[updateModule] unable to set the feedback.";
                return false;
            }

            if(!m_walkingController->setReferenceSignal(m_DCMPositionDesired, resetTrajectory))
            {
                yError() << "[updateModule] unable to set the reference Signal.";
                return false;
            }

            if(!m_walkingController->solve())
            {
                yError() << "[updateModule] Unable to solve the problem.";
                return false;
            }

            if(!m_walkingController->getControllerOutput(desiredZMP))
            {
                yError() << "[updateModule] Unable to get the MPC output.";
                return false;
            }

            m_profiler->setEndTime("MPC");
        }
        else
        {
            m_walkingDCMReactiveController->setFeedback(measuredDCM);
            m_walkingDCMReactiveController->setReferenceSignal(m_DCMPositionDesired.front(),
                                                               m_DCMVelocityDesired.front());

            if(!m_walkingDCMReactiveController->evaluateControl())
            {
                yError() << "[updateModule] Unable to evaluate the DCM control output.";
                return false;
            }

            if(!m_walkingDCMReactiveController->getControllerOutput(desiredZMP))
            {
                yError() << "[updateModule] Unable to get the DCM control output.";
                return false;
            }
        }

        // inner COM-ZMP controller
        // if the the norm of desired DCM velocity is lower than a threshold then the robot
        // is stopped
        double threshold = 0.001;
        bool stancePhase = iDynTree::toEigen(m_DCMVelocityDesired.front()).norm() < threshold;
        m_walkingZMPController->setPhase(stancePhase);

        // set feedback and the desired signal
        m_walkingZMPController->setFeedback(measuredZMP, measuredCoM);
        m_walkingZMPController->setReferenceSignal(desiredZMP, desiredCoMPositionXY,
                                                   desiredCoMVelocityXY);

        if(!m_walkingZMPController->evaluateControl())
        {
            yError() << "[updateModule] Unable to evaluate the ZMP control output.";
            return false;
        }

        iDynTree::Vector2 outputZMPCoMControllerPosition, outputZMPCoMControllerVelocity;
        if(!m_walkingZMPController->getControllerOutput(outputZMPCoMControllerPosition,
                                                        outputZMPCoMControllerVelocity))
        {
            yError() << "[updateModule] Unable to get the ZMP controller output.";
            return false;
        }

        // inverse kinematics
        m_profiler->setInitTime("IK");

        iDynTree::Position desiredCoMPosition;
        desiredCoMPosition(0) = outputZMPCoMControllerPosition(0);
        desiredCoMPosition(1) = outputZMPCoMControllerPosition(1);
        desiredCoMPosition(2) = m_comHeightTrajectory.front();


        iDynTree::Vector3 desiredCoMVelocity;
        desiredCoMVelocity(0) = outputZMPCoMControllerVelocity(0);
        desiredCoMVelocity(1) = outputZMPCoMControllerVelocity(1);
        desiredCoMVelocity(2) = m_comHeightVelocity.front();

        // evaluate desired neck transformation
        double yawLeft = m_leftTrajectory.front().getRotation().asRPY()(2);
        double yawRight = m_rightTrajectory.front().getRotation().asRPY()(2);

        double meanYaw = std::atan2(std::sin(yawLeft) + std::sin(yawRight),
                                    std::cos(yawLeft) + std::cos(yawRight));
        iDynTree::Rotation yawRotation, modifiedInertial;

        yawRotation = iDynTree::Rotation::RotZ(meanYaw);
        yawRotation = yawRotation.inverse();
        modifiedInertial = yawRotation * m_inertial_R_worldFrame;

        if(m_useQPIK)
        {
            // integrate dq because velocity control mode seems not available
            yarp::sig::Vector bufferVelocity(m_robotControlHelper->getActuatedDoFs());
            yarp::sig::Vector bufferPosition(m_robotControlHelper->getActuatedDoFs());

            if(!m_FKSolver->setInternalRobotState(m_qDesired, m_dqDesired))
            {
                yError() << "[updateModule] Unable to set the internal robot state.";
                return false;
            }

            if(m_useOSQP)
            {
                if(!solveQPIK(m_QPIKSolver_osqp, desiredCoMPosition,
                              desiredCoMVelocity, measuredCoM,
                              yawRotation, m_dqDesired))
                {
                    yError() << "[updateModule] Unable to solve the QP problem with osqp.";
                    return false;
                }
            }
            else
            {
                if(!solveQPIK(m_QPIKSolver_qpOASES, desiredCoMPosition,
                              desiredCoMVelocity, measuredCoM,
                              yawRotation, m_dqDesired))
                {
                    yError() << "[updateModule] Unable to solve the QP problem with osqp.";
                    return false;
                }
            }
            iDynTree::toYarp(m_dqDesired, bufferVelocity);

            bufferPosition = m_velocityIntegral->integrate(bufferVelocity);
            iDynTree::toiDynTree(bufferPosition, m_qDesired);
        }
        else
        {
            if(m_IKSolver->usingAdditionalRotationTarget())
            {
                if(!m_IKSolver->updateIntertiaToWorldFrameRotation(modifiedInertial))
                {
                    yError() << "[updateModule] Error updating the inertia to world frame rotation.";
                    return false;
                }

                if(!m_IKSolver->setFullModelFeedBack(m_robotControlHelper->getJointPosition()))
                {
                    yError() << "[updateModule] Error while setting the feedback to the inverse Kinematics.";
                    return false;
                }

                if(!m_IKSolver->computeIK(m_leftTrajectory.front(), m_rightTrajectory.front(),
                                          desiredCoMPosition, m_qDesired))
                {
                    yError() << "[updateModule] Error during the inverse Kinematics iteration.";
                    return false;
                }
            }
        }
        m_profiler->setEndTime("IK");

        if(!m_robotControlHelper->setDirectPositionReferences(m_qDesired))
        {
            yError() << "[updateModule] Error while setting the reference position to iCub.";
            return false;
        }

        m_profiler->setEndTime("Total");

        // print timings
        m_profiler->profiling();

        iDynTree::VectorDynSize errorL(6), errorR(6);
        if(m_useQPIK)
        {
            if(m_useOSQP)
            {
                m_QPIKSolver_osqp->getRightFootError(errorR);
                m_QPIKSolver_osqp->getLeftFootError(errorL);
            }
            else
            {
                m_QPIKSolver_qpOASES->getRightFootError(errorR);
                m_QPIKSolver_qpOASES->getLeftFootError(errorL);
            }
        }

        // send data to the WalkingLogger
        if(m_dumpData)
        {
            auto leftFoot = m_FKSolver->getLeftFootToWorldTransform();
            auto rightFoot = m_FKSolver->getRightFootToWorldTransform();
            m_walkingLogger->sendData(measuredDCM, m_DCMPositionDesired.front(), m_DCMVelocityDesired.front(),
                                      measuredZMP, desiredZMP, measuredCoM,
                                      desiredCoMPositionXY, desiredCoMVelocityXY,
                                      leftFoot.getPosition(), leftFoot.getRotation().asRPY(),
                                      rightFoot.getPosition(), rightFoot.getRotation().asRPY(),
                                      m_leftTrajectory.front().getPosition(), m_leftTrajectory.front().getRotation().asRPY(),
                                      m_rightTrajectory.front().getPosition(), m_rightTrajectory.front().getRotation().asRPY(),
                                      errorL, errorR);
        }

        propagateTime();

        // advance all the signals
        advanceReferenceSignals();

        if(m_firstStep)
            m_firstStep = false;

    }
    return true;
}

//for zmp and CoM Velocity filter resetting
bool WalkingModule::resetZMPFilters()
{
    if(!m_robotControlHelper->getFeedbacksRaw(10))
    {
        yError() << "[resetZMPFilters] Unable to get the feedback from the robot";
        return false;
    }

    if(!updateFKSolver())
    {
        yError() << "[resetZMPFilters] Unable to update the FK solver.";
        return false;
    }


    yarp::sig::Vector temp1ZMPFilter;
    iDynTree::Vector2 temp1MeasuredZMP;

    if(!evaluateZMP(temp1MeasuredZMP))
    {
        yError() << "[resetZMPFilters] Unable to evaluate the ZMP.";
        return false;
    }
    if(m_useZMPFilter)
    {
        iDynTree::toYarp(temp1MeasuredZMP,temp1ZMPFilter);
        m_ZMPFilter->init(temp1ZMPFilter);
    }

    return true;
}


bool WalkingModule::evaluateZMP(iDynTree::Vector2& zmp)
{
    if(m_FKSolver == nullptr)
    {
        yError() << "[evaluateZMP] The FK solver is not ready.";
        return false;
    }

    iDynTree::Position zmpLeft, zmpRight, zmpWorld;
    double zmpLeftDefined = 0.0, zmpRightDefined = 0.0;

    const iDynTree::Wrench& rightWrench = m_robotControlHelper->getRightWrench();
    const iDynTree::Wrench& leftWrench = m_robotControlHelper->getLeftWrench();

    if(m_useZMPSaturation==1){
        double saturatedRFz=rightWrench.getLinearVec3()(2);
        double saturatedLFz=leftWrench.getLinearVec3()(2);

        if(rightWrench.getLinearVec3()(2) < thresholdFz)
            zmpRightDefined = 0.0;
        else
        {
            if (saturateFz(saturatedRFz,thresholdFz)) {
                zmpRight(0) = (-rightWrench.getAngularVec3()(1)*saturatedRFz) / ((saturatedRFz*saturatedRFz)+epsilonZMP);
                zmpRight(1) = (rightWrench.getAngularVec3()(0)*saturatedRFz) / ((saturatedRFz*saturatedRFz)+epsilonZMP);
                //            zmpRight(0) = -rightWrench.getAngularVec3()(1) / rightWrench.getLinearVec3()(2);
                //            zmpRight(1) = rightWrench.getAngularVec3()(0) / rightWrench.getLinearVec3()(2);
                zmpRight(2) = 0.0;
                zmpRightDefined = 1.0;
            }
            else {
                yError() << "[evaluateZMP] The saturation function can not saturate Fz in right foot!!!!";
            }
        }


        if(leftWrench.getLinearVec3()(2) < thresholdFz)
            zmpLeftDefined = 0.0;
        else
        {
            if (saturateFz(saturatedLFz,thresholdFz)) {
                zmpLeft(0) = (-leftWrench.getAngularVec3()(1)*saturatedLFz) / ((saturatedLFz*saturatedLFz)+epsilonZMP);
                zmpLeft(1) = (leftWrench.getAngularVec3()(0)*saturatedLFz) / ((saturatedLFz*saturatedLFz)+epsilonZMP);
                //            zmpLeft(0) = -leftWrench.getAngularVec3()(1) / leftWrench.getLinearVec3()(2);
                //            zmpLeft(1) = leftWrench.getAngularVec3()(0) / leftWrench.getLinearVec3()(2);

                zmpLeft(2) = 0.0;
                zmpLeftDefined = 1.0;
            }
            else {
                yError() << "[evaluateZMP] The saturation function can not saturate Fz in left foot!!!!";
            }
        }

        double totalZ = saturatedLFz + saturatedRFz;
        if(totalZ < 0.1)
        {
            yError() << "[evaluateZMP] The total z-component of contact wrenches is too low.";
            return false;
        }

        zmpLeft = m_FKSolver->getLeftFootToWorldTransform() * zmpLeft;
        zmpRight = m_FKSolver->getRightFootToWorldTransform() * zmpRight;

        // the global zmp is given by a weighted average
        iDynTree::toEigen(zmpWorld) = ((saturatedLFz * zmpLeftDefined) / totalZ)
                * iDynTree::toEigen(zmpLeft) +
                ((saturatedRFz * zmpRightDefined)/totalZ) * iDynTree::toEigen(zmpRight);
    }
    else{//when the saturation is not active

        if(rightWrench.getLinearVec3()(2) < 0.001)
            zmpRightDefined = 0.0;
        else
        {
            zmpRight(0) = -rightWrench.getAngularVec3()(1) / rightWrench.getLinearVec3()(2);
            zmpRight(1) = rightWrench.getAngularVec3()(0) / rightWrench.getLinearVec3()(2);
            zmpRight(2) = 0.0;
            zmpRightDefined = 1.0;
        }

        if(leftWrench.getLinearVec3()(2) < 0.001)
            zmpLeftDefined = 0.0;
        else
        {
            zmpLeft(0) = -leftWrench.getAngularVec3()(1) / leftWrench.getLinearVec3()(2);
            zmpLeft(1) = leftWrench.getAngularVec3()(0) / leftWrench.getLinearVec3()(2);
            zmpLeft(2) = 0.0;
            zmpLeftDefined = 1.0;
        }

        double totalZ = rightWrench.getLinearVec3()(2) + leftWrench.getLinearVec3()(2);
        if(totalZ < 0.1)
        {
            yError() << "[evaluateZMP] The total z-component of contact wrenches is too low.";
            return false;
        }

        zmpLeft = m_FKSolver->getLeftFootToWorldTransform() * zmpLeft;
        zmpRight = m_FKSolver->getRightFootToWorldTransform() * zmpRight;

        // the global zmp is given by a weighted average
        iDynTree::toEigen(zmpWorld) = ((leftWrench.getLinearVec3()(2) * zmpLeftDefined) / totalZ)
                * iDynTree::toEigen(zmpLeft) +
                ((rightWrench.getLinearVec3()(2) * zmpRightDefined)/totalZ) * iDynTree::toEigen(zmpRight);
    }
    zmp(0) = zmpWorld(0);
    zmp(1) = zmpWorld(1);

    return true;
}

bool WalkingModule::saturateFz(double& Fz,const double thresholdFz)
{
    if (thresholdFz<0){
        yError() << "[saturateFz] The threshold for the saturation of Fz must be greater than zero";
        return false;
    }

    if (Fz>=thresholdFz) {
        return true;
    }
    else if (Fz<thresholdFz) {
        Fz=0;
    }
    return true;
}


bool WalkingModule::prepareRobot(bool onTheFly)
{
    if(m_robotState != WalkingFSM::Configured && m_robotState != WalkingFSM::Stopped)
    {
        yError() << "[prepareRobot] The robot can be prepared only at the "
                 << "beginning or when the controller is stopped.";
        return false;
    }

    iDynTree::Position measuredCoM;
    iDynTree::Vector3 measuredCoMVelocity;
    iDynTree::Transform leftToRightTransform;

    // get the current state of the robot
    // this is necessary because the trajectories for the joints, CoM height and neck orientation
    // depend on the current state of the robot
    if(!m_robotControlHelper->getFeedbacksRaw(10))
    {
        yError() << "[prepareRobot] Unable to get the feedback.";
        return false;
    }

    if(onTheFly)
    {
        if(!m_FKSolver->setBaseOnTheFly())
        {
            yError() << "[prepareRobot] Unable to set the onTheFly base.";
            return false;
        }

        if(!m_FKSolver->setInternalRobotState(m_robotControlHelper->getJointPosition(),
                                              m_robotControlHelper->getJointVelocity()))
        {
            yError() << "[prepareRobot] Unable to evaluate the CoM.";
            return false;
        }

        // evaluate the left to right transformation, the inertial frame is on the left foot
        leftToRightTransform = m_FKSolver->getRightFootToWorldTransform();

        // evaluate the first trajectory. The robot does not move!
        if(!generateFirstTrajectories(leftToRightTransform))
        {
            yError() << "[prepareRobot] Failed to evaluate the first trajectories.";
            return false;
        }
    }
    else
    {
        // evaluate the first trajectory. The robot does not move! So the first trajectory
        if(!generateFirstTrajectories())
        {
            yError() << "[prepareRobot] Failed to evaluate the first trajectories.";
            return false;
        }
    }

    // reset the gains
    if (m_robotControlHelper->getPIDHandler().usingGainScheduling())
    {
        if (!(m_robotControlHelper->getPIDHandler().reset()))
            return false;
    }

    if(!m_IKSolver->setFullModelFeedBack(m_robotControlHelper->getJointPosition()))
    {
        yError() << "[prepareRobot] Error while setting the feedback to the IK solver.";
        return false;
    }

    iDynTree::Position desiredCoMPosition;
    desiredCoMPosition(0) = m_DCMPositionDesired.front()(0);
    desiredCoMPosition(1) = m_DCMPositionDesired.front()(1);
    desiredCoMPosition(2) = m_comHeightTrajectory.front();

    if(m_IKSolver->usingAdditionalRotationTarget())
    {
        // get the yow angle of both feet
        double yawLeft = m_leftTrajectory.front().getRotation().asRPY()(2);
        double yawRight = m_rightTrajectory.front().getRotation().asRPY()(2);

        // evaluate the mean of the angles
        double meanYaw = std::atan2(std::sin(yawLeft) + std::sin(yawRight),
                                    std::cos(yawLeft) + std::cos(yawRight));
        iDynTree::Rotation yawRotation, modifiedInertial;

        // it is important to notice that the inertial frames rotate with the robot
        yawRotation = iDynTree::Rotation::RotZ(meanYaw);

        yawRotation = yawRotation.inverse();
        modifiedInertial = yawRotation * m_inertial_R_worldFrame;

        if(!m_IKSolver->updateIntertiaToWorldFrameRotation(modifiedInertial))
        {
            yError() << "[prepareRobot] Error updating the inertia to world frame rotation.";
            return false;
        }
    }

    if(!m_IKSolver->computeIK(m_leftTrajectory.front(), m_rightTrajectory.front(),
                              desiredCoMPosition, m_qDesired))
    {
        yError() << "[prepareRobot] Inverse Kinematics failed while computing the initial position.";
        return false;
    }

    if(!m_robotControlHelper->switchToControlMode(VOCAB_CM_POSITION))
    {
        yError() << "[prepareRobot] Error while setting the position control.";
        return false;
    }

    if(!m_robotControlHelper->setPositionReferences(m_qDesired, 5.0))
    {
        yError() << "[prepareRobot] Error while setting the initial position.";
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_robotState = WalkingFSM::Preparing;
    }


    return true;
}

bool WalkingModule::generateFirstTrajectories(const iDynTree::Transform &leftToRightTransform)
{
    if(m_trajectoryGenerator == nullptr)
    {
        yError() << "[generateFirstTrajectories] Unicycle planner not available.";
        return false;
    }

    if(!m_trajectoryGenerator->generateFirstTrajectories(leftToRightTransform))
    {
        yError() << "[generateFirstTrajectories] Failed while retrieving new trajectories from the unicycle";
        return false;
    }

    if(!updateTrajectories(0))
    {
        yError() << "[generateFirstTrajectories] Unable to update the trajectory.";
        return false;
    }

    // reset the time
    m_time = 0.0;

    return true;
}

bool WalkingModule::generateFirstTrajectories()
{
    if(m_trajectoryGenerator == nullptr)
    {
        yError() << "[generateFirstTrajectories] Unicycle planner not available.";
        return false;
    }

    if(!m_trajectoryGenerator->generateFirstTrajectories())
    {
        yError() << "[generateFirstTrajectories] Failed while retrieving new trajectories from the unicycle";
        return false;
    }

    if(!updateTrajectories(0))
    {
        yError() << "[generateFirstTrajectories] Unable to update the trajectory.";
        return false;
    }

    // reset the time
    m_time = 0.0;

    return true;
}

bool WalkingModule::askNewTrajectories(const double& initTime, const bool& isLeftSwinging,
                                       const iDynTree::Transform& measuredTransform,
                                       const size_t& mergePoint, const iDynTree::Vector2& desiredPosition)
{
    if(m_trajectoryGenerator == nullptr)
    {
        yError() << "[askNewTrajectories] Unicycle planner not available.";
        return false;
    }

    if(mergePoint >= m_DCMPositionDesired.size())
    {
        yError() << "[askNewTrajectories] The mergePoint has to be lower than the trajectory size.";
        return false;
    }

    if(!m_trajectoryGenerator->updateTrajectories(initTime, m_DCMPositionDesired[mergePoint],
                                                  m_DCMVelocityDesired[mergePoint], isLeftSwinging,
                                                  measuredTransform, desiredPosition))
    {
        yError() << "[askNewTrajectories] Unable to update the trajectory.";
        return false;
    }
    return true;
}

bool WalkingModule::updateTrajectories(const size_t& mergePoint)
{
    if(!(m_trajectoryGenerator->isTrajectoryComputed()))
    {
        yError() << "[updateTrajectories] The trajectory is not computed.";
        return false;
    }

    std::vector<iDynTree::Transform> leftTrajectory;
    std::vector<iDynTree::Transform> rightTrajectory;
    std::vector<iDynTree::Twist> leftTwistTrajectory;
    std::vector<iDynTree::Twist> rightTwistTrajectory;
    std::vector<iDynTree::Vector2> DCMPositionDesired;
    std::vector<iDynTree::Vector2> DCMVelocityDesired;
    std::vector<bool> rightInContact;
    std::vector<bool> leftInContact;
    std::vector<double> comHeightTrajectory;
    std::vector<double> comHeightVelocity;
    std::vector<size_t> mergePoints;
    std::vector<bool> isLeftFixedFrame;

    // get dcm position and velocity
    m_trajectoryGenerator->getDCMPositionTrajectory(DCMPositionDesired);
    m_trajectoryGenerator->getDCMVelocityTrajectory(DCMVelocityDesired);

    // get feet trajectories
    m_trajectoryGenerator->getFeetTrajectories(leftTrajectory, rightTrajectory);
    m_trajectoryGenerator->getFeetTwist(leftTwistTrajectory, rightTwistTrajectory);
    m_trajectoryGenerator->getFeetStandingPeriods(leftInContact, rightInContact);
    m_trajectoryGenerator->getWhenUseLeftAsFixed(isLeftFixedFrame);

    // get com height trajectory
    m_trajectoryGenerator->getCoMHeightTrajectory(comHeightTrajectory);
    m_trajectoryGenerator->getCoMHeightVelocity(comHeightVelocity);

    // get merge points
    m_trajectoryGenerator->getMergePoints(mergePoints);

    // append vectors to deques
    StdHelper::appendVectorToDeque(leftTrajectory, m_leftTrajectory, mergePoint);
    StdHelper::appendVectorToDeque(rightTrajectory, m_rightTrajectory, mergePoint);
    StdHelper::appendVectorToDeque(leftTwistTrajectory, m_leftTwistTrajectory, mergePoint);
    StdHelper::appendVectorToDeque(rightTwistTrajectory, m_rightTwistTrajectory, mergePoint);
    StdHelper::appendVectorToDeque(isLeftFixedFrame, m_isLeftFixedFrame, mergePoint);

    StdHelper::appendVectorToDeque(DCMPositionDesired, m_DCMPositionDesired, mergePoint);
    StdHelper::appendVectorToDeque(DCMVelocityDesired, m_DCMVelocityDesired, mergePoint);

    StdHelper::appendVectorToDeque(leftInContact, m_leftInContact, mergePoint);
    StdHelper::appendVectorToDeque(rightInContact, m_rightInContact, mergePoint);

    StdHelper::appendVectorToDeque(comHeightTrajectory, m_comHeightTrajectory, mergePoint);
    StdHelper::appendVectorToDeque(comHeightVelocity, m_comHeightVelocity, mergePoint);

    m_mergePoints.assign(mergePoints.begin(), mergePoints.end());

    // the first merge point is always equal to 0
    m_mergePoints.pop_front();
    m_mergePoints.size();
    return true;
}

bool WalkingModule::updateFKSolver()
{
    if(!m_FKSolver->evaluateWorldToBaseTransformation(m_leftTrajectory.front(),
                                                      m_rightTrajectory.front(),
                                                      m_isLeftFixedFrame.front()))
    {
        yError() << "[updateFKSolver] Unable to evaluate the world to base transformation.";
        return false;
    }

    if(!m_FKSolver->setInternalRobotState(m_robotControlHelper->getJointPosition(),
                                          m_robotControlHelper->getJointVelocity()))
    {
        yError() << "[updateFKSolver] Unable to evaluate the CoM.";
        return false;
    }

    return true;
}

bool WalkingModule::evaluateDesiredStepData(){


}
bool WalkingModule::evaluateCoM(iDynTree::Position& comPosition, iDynTree::Vector3& comVelocity)
{
    if(m_FKSolver == nullptr)
    {
        yError() << "[evaluateCoM] The FK solver is not ready.";
        return false;
    }

    if(!m_FKSolver->evaluateCoM())
    {
        yError() << "[evaluateCoM] Unable to evaluate the CoM.";
        return false;
    }

    if(!m_FKSolver->getCoMPosition(comPosition))
    {
        yError() << "[evaluateCoM] Unable to get the CoM position.";
        return false;
    }

    if(!m_FKSolver->getCoMVelocity(comVelocity))
    {
        yError() << "[evaluateCoM] Unable to get the CoM velocity.";
        return false;
    }

    return true;
}

bool WalkingModule::evaluateDCM(iDynTree::Vector2& dcm)
{
    if(m_FKSolver == nullptr)
    {
        yError() << "[evaluateDCM] The FK solver is not ready.";
        return false;
    }

    if(!m_FKSolver->evaluateDCM())
    {
        yError() << "[evaluateDCM] Unable to evaluate the DCM.";
        return false;
    }

    if(!m_FKSolver->getDCM(dcm))
    {
        yError() << "[evaluateDCM] Unable to get the DCM.";
        return false;
    }

    return true;
}

bool WalkingModule::startWalking()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if(m_robotState != WalkingFSM::Prepared && m_robotState != WalkingFSM::Paused)
    {
        yError() << "[startWalking] Unable to start walking if the robot is not prepared or paused.";
        return false;
    }

    if(m_dumpData)
    {
        m_walkingLogger->startRecord({"record","dcm_x", "dcm_y",
                                      "dcm_des_x", "dcm_des_y",
                                      "dcm_des_dx", "dcm_des_dy",
                                      "zmp_x", "zmp_y",
                                      "zmp_des_x", "zmp_des_y",
                                      "com_x", "com_y", "com_z",
                                      "com_des_x", "com_des_y",
                                      "com_des_dx", "com_des_dy",
                                      "lf_x", "lf_y", "lf_z",
                                      "lf_roll", "lf_pitch", "lf_yaw",
                                      "rf_x", "rf_y", "rf_z",
                                      "rf_roll", "rf_pitch", "rf_yaw",
                                      "lf_des_x", "lf_des_y", "lf_des_z",
                                      "lf_des_roll", "lf_des_pitch", "lf_des_yaw",
                                      "rf_des_x", "rf_des_y", "rf_des_z",
                                      "rf_des_roll", "rf_des_pitch", "rf_des_yaw",
                                      "lf_err_x", "lf_err_y", "lf_err_z",
                                      "lf_err_roll", "lf_err_pitch", "lf_err_yaw",
                                      "rf_err_x", "rf_err_y", "rf_err_z",
                                      "rf_err_roll", "rf_err_pitch", "rf_err_yaw"});
    }

    // if the robot was only prepared the filters has to be reseted
    if(m_robotState == WalkingFSM::Prepared){
        m_robotControlHelper->resetFilters();
        resetZMPFilters();
    }



    m_robotState = WalkingFSM::Walking;
    m_firstStep = true;

    return true;
}

bool WalkingModule::setPlannerInput(double x, double y)
{
    // the trajectory was already finished the new trajectory will be attached as soon as possible
    if(m_mergePoints.empty())
    {
        if(!(m_leftInContact.front() && m_rightInContact.front()))
        {
            yError() << "[setGoal] The trajectory has already finished but the system is not in double support.";
            return false;
        }
        if(m_newTrajectoryRequired)
            return true;

        // Since the evaluation of a new trajectory takes time the new trajectory will be merged after x cycles
        m_newTrajectoryMergeCounter = 20;
    }

    // the trajectory was not finished the new trajectory will be attached at the next merge point
    else
    {
        if(m_mergePoints.front() > 20){
            m_newTrajectoryMergeCounter = m_mergePoints.front();
        }
        else if(m_mergePoints.size() > 1)
        {

            if(m_newTrajectoryRequired)
                return true;

            m_newTrajectoryMergeCounter = m_mergePoints[1];
        }
        else
        {

            if(m_newTrajectoryRequired)
                return true;

            m_newTrajectoryMergeCounter = 20;

        }
    }

    m_desiredPosition(0) = x;
    m_desiredPosition(1) = y;

    m_newTrajectoryRequired = true;

    return true;
}

bool WalkingModule::setGoal(double x, double y)
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if(m_robotState != WalkingFSM::Walking)
        return false;

    return setPlannerInput(x, y);
}

bool WalkingModule::pauseWalking()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if(m_robotState != WalkingFSM::Walking)
        return false;

    // close the logger
    if(m_dumpData)
        m_walkingLogger->quit();

    m_robotState = WalkingFSM::Paused;
    return true;
}


bool WalkingModule::stopWalking()
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if(m_robotState != WalkingFSM::Walking)
        return false;

    reset();

    m_robotState = WalkingFSM::Stopped;
    return true;
}

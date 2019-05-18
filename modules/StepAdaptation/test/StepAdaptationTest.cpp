/*
 * Copyright (C) 2017 Fondazione Istituto Italiano di Tecnologia
 * Authors: Stefano Dafarra
 * CopyPolicy: Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 *
 */


#include <iDynTree/Core/TestUtils.h>
#include <iDynTree/Core/Utils.h>
#include <Eigen/Core>
#include "iDynTree/Core/EigenHelpers.h"
#include "iDynTree/Core/MatrixDynSize.h"
#include <cmath>
#include <memory>
#include <iostream>
#include <ctime>
#include <yarp/os/LogStream.h>
#include <yarp/os/Network.h>
#include <yarp/os/RFModule.h>
#include <yarp/sig/Vector.h>
#include<StepAdaptator.hpp>
#include <yarp/os/RpcClient.h>
#include <WalkingLogger.hpp>

#include <yarp/os/BufferedPort.h>
#include <yarp/os/LogStream.h>



int main(int argc, char **argv) {

    std::unique_ptr<WalkingLogger> m_walkingLogger; /**< Pointer to the Walking Logger object. */

    // prepare and configure the resource finder
    // initialise yarp network
    yarp::os::Network yarp;
    if (!yarp.checkNetwork())
    {
        yError()<<"[main] Unable to find YARP network";
        return EXIT_FAILURE;
    }

    std::unique_ptr<StepAdaptator> m_stepAdaptator;
    yarp::os::ResourceFinder& rf = yarp::os::ResourceFinder::getResourceFinderSingleton();
    rf.setDefaultConfigFile("dcmWalkingCoordinator.ini");


    yarp::os::Bottle& generalOptions = rf.findGroup("GENERAL");
    double  m_dT = generalOptions.check("sampling_time", yarp::os::Value(0.016)).asDouble();



    rf.configure(argc, argv);

    m_walkingLogger = std::make_unique<WalkingLogger>();
    yarp::os::Bottle& loggerOptions = rf.findGroup("WALKING_LOGGER");
    if(!m_walkingLogger->configure(loggerOptions, "stepAdaptation"))
    {
        yError() << "[configure] Unable to configure the logger.";
        return false;
    }

    m_walkingLogger->startRecord({"record","foot_pos_x","stepTiming","DCM_offset_x","timed1","timed2","nomNextStep","nomStepTiming","nomDCMOffset","nomLastDCM","omega","var1","var2","var3","var4"});
    // initialize the step adaptation
    m_stepAdaptator = std::make_unique<StepAdaptator>();
    yarp::os::Bottle& stepAdaptatorOptions = rf.findGroup("STEP_ADAPTATOR");
    stepAdaptatorOptions.append(generalOptions);
    iDynTree::VectorFixSize<5> nominalValues;
    iDynTree::Vector3 currentValues;
    double a=0;
    double b=0.1;
    double c;
    double omega=sqrt(9.81/0.6);
    int i=0;
    double stepTiming=0.5;
    double sigma;
    double nextStepPosition=0.5;
    double nominalDCMOffset;
    iDynTree::Vector2 timed;
    timed(0)=0;
    iDynTree::Vector3 leftAdaptedStepParameters;


    for(int var=1;var<=1000;var++){
        i++;

        sigma=exp(omega*stepTiming);

        //nomStepTiming=(jLeftstepList.at(1).impactTime-jRightstepList.at(0).impactTime)/(1+switchOverSwingRatio);
        nominalDCMOffset=0.5/(exp(omega*stepTiming)-1);

        nominalValues(0)=nextStepPosition;
        nominalValues(1)=sigma;
        nominalValues(2)=nominalDCMOffset;
        nominalValues(3)=0;
        nominalValues(4)=omega;


        currentValues(0)=a;
        currentValues(1)=b;
        currentValues(2)=0;

        if(i==122){
            b=b+0.12;
        }

        if(i==165){
            b=b-0.12;
        }

        if (((i+1)%100)==0) {
            a=a+0.5;
            b=b+0.5;
            stepTiming=0.5;
            nextStepPosition=nextStepPosition+0.5;
        }


        //b=b+0.005;
        timed(0)=timed(0)+0.01;
        // stepTiming=stepTiming-0.01;





        if(!m_stepAdaptator->initialize(stepAdaptatorOptions))
        {
            yError() << "[configure] Unable to initialize the step adaptator!";
            return false;
        }



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
        double mil=leftAdaptedStepParameters(0)+leftAdaptedStepParameters(2)+(currentValues(0)-currentValues(1))*(leftAdaptedStepParameters(1))-currentValues(0);
        iDynTree::Vector4 tempp;
        tempp(0)=mil;
        tempp(1)=(currentValues(0)-currentValues(1));
        tempp(2)=leftAdaptedStepParameters(1);
        tempp(3)=currentValues(0);

        m_walkingLogger->sendData(leftAdaptedStepParameters,timed,nominalValues,tempp);


        yarp::os::Time::delay(0.01);
    }

    m_walkingLogger->quit();
    return EXIT_SUCCESS;
}
/*
Copyright (c) Microsoft Corporation

All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in 
compliance with the License.  You may obtain a copy of the License 
at http://www.apache.org/licenses/LICENSE-2.0   


THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER 
EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF 
TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.  


See the Apache Version 2.0 License for specific language governing permissions and 
limitations under the License. 

*/

#pragma once

DRDECLARECLASS(DrCluster);
DRREF(DrCluster);

// DrProcessState is also in Java class DryadAppMaster 
DRENUM(DrProcessState)
{
    DPS_NotStarted,
    DPS_Initializing,
    DPS_Scheduling,
    DPS_Starting,
    DPS_Created,
    DPS_Running,
    DPS_Completed,
    DPS_Failed,
    DPS_Zombie
};

DRENUM(DrProcessBasicState)
{
    DPBS_NotStarted,
    DPBS_Running,
    DPBS_Completed,
    DPBS_Failed
};

DRBASECLASS(DrProcessHandle abstract)
{
public:
    virtual ~DrProcessHandle();

    virtual void CloseHandle() DRABSTRACT;
    virtual DrString GetHandleIdAsString() DRABSTRACT;
    virtual DrString GetDirectory() DRABSTRACT;
    virtual DrResourcePtr GetAssignedNode() DRABSTRACT;
};
DRREF(DrProcessHandle);

DRDECLARECLASS(DrProcessInfo);
DRREF(DrProcessInfo);

typedef DrListener<DrProcessInfoRef> DrProcessListener;
DRIREF(DrProcessListener);
DRMAKEARRAYLIST(DrProcessListenerIRef);

DRBASECLASS(DrProcessTemplate)
{
public:
    DrProcessTemplate();

    void SetCommandLineBase(DrString commandLine);
    DrString GetCommandLineBase();

    void SetProcessClass(DrString processClass);
    DrString GetProcessClass();

    DrProcessListenerIRefListPtr GetListenerList();

    void SetFailedRetainAndLeaseGraceTime(DrTimeInterval time,
                                          DrTimeInterval leaseGraceTime);
    DrTimeInterval GetFailedRetainTime();
    DrTimeInterval GetFailedLeaseWaitTime();
    void SetCompletedRetainAndLeaseGraceTime(DrTimeInterval time,
                                             DrTimeInterval leaseGraceTime);
    DrTimeInterval GetCompletedRetainTime();
    DrTimeInterval GetCompletedLeaseWaitTime();

    void SetMaxMemory(UINT64 maxMemory);
    UINT64 GetMaxMemory();

    void SetTimeOutBetweenProcessEndAndVertexNotification(DrTimeInterval timeOut);
    DrTimeInterval GetTimeOutBetweenProcessEndAndVertexNotification();

    DrFloatArrayPtr GetAffinityLevelThresholds();

private:
    DrString                       m_commandLineBase;
    DrString                       m_processClass;

    DrProcessListenerIRefListRef   m_listenerList;

    DrTimeInterval                 m_failedRetainTime;
    DrTimeInterval                 m_failedLeaseGraceTime;
    DrTimeInterval                 m_completedRetainTime;
    DrTimeInterval                 m_completedLeaseGraceTime;

    UINT64                         m_maxMemory;

    DrTimeInterval                 m_timeOutBetweenProcessEndAndVertexNotification;

    DrFloatArrayRef                m_affinityLevelThresholds;
};
DRREF(DrProcessTemplate);

DRDECLARECLASS(DrProcessStateRecord);
DRREF(DrProcessStateRecord);
DRBASECLASS(DrProcessStateRecord)
{
public:
    DrProcessStateRecord();
    DrProcessStateRecordRef Clone();
    void Assimilate(DrProcessStateRecordPtr newState);

    DrProcessHandleRef  m_process;
    DrProcessState      m_state;
    UINT32              m_exitCode;
    DrErrorRef          m_status;

    DrDateTime          m_creatingTime;
    DrDateTime          m_createdTime;
    DrDateTime          m_beginExecutionTime;
    DrDateTime          m_terminatedTime;

    /* currently not being collected, but easy to add
    UINT32              m_pid;
    DrTimeInterval      m_userTime;
    DrTimeInterval      m_kernelTime;
    INT32               m_pageFaults;
    UINT64              m_peakVMUsage;    
    UINT64              m_peakMemUsage;
    UINT64              m_memUsageSeconds;
    UINT64              m_totalIO;    
    */
};

DRDECLARECLASS(DrProcess);
DRREF(DrProcess);

DRBASECLASS(DrProcessInfo)
{
public:
    DrLockBox<DrProcess>        m_process;
    DrProcessStateRecordRef     m_state;

    DrDateTime                  m_jmProcessCreatedTime;
    DrDateTime                  m_jmProcessScheduledTime;
};
DRREF(DrProcessInfo);

typedef DrListener<DrProcessStateRecordRef> DrPSRListener;
DRIREF(DrPSRListener);

typedef DrMessage<DrProcessStateRecordRef> DrPSRMessage;
DRREF(DrPSRMessage);

typedef DrMessage<DrProcessInfoRef> DrProcessMessage;
DRREF(DrProcessMessage);

typedef DrNotifier<DrProcessInfoRef> DrProcessNotifier;

DRBASECLASS(DrPropertyStatus)
{
public:
    DrPropertyStatus(DrProcessBasicState state, UINT32 exitCode, DrErrorPtr error);

    DrProcessBasicState     m_processState;
    UINT32                  m_exitCode;
    DrErrorRef              m_status;
    DrLockBox<DrProcess>    m_process;
    UINT64                  m_statusVersion;
    DrByteArrayRef          m_statusBlock;
};
DRREF(DrPropertyStatus);

typedef DrListener<DrPropertyStatusRef> DrPropertyListener;
DRIREF(DrPropertyListener);

typedef DrMessage<DrPropertyStatusRef> DrPropertyMessage;
DRREF(DrPropertyMessage);

typedef DrNotifier<DrPropertyStatusRef> DrPropertyNotifier;

typedef DrListener<DrProcessState> DrPStateListener;
DRIREF(DrPStateListener);

typedef DrMessage<DrProcessState> DrPStateMessage;
DRREF(DrPStateMessage);

DRCLASS(DrProcess)
    : public DrProcessNotifier, public DrPSRListener, public DrErrorListener, public DrPStateListener
{
public:
    DrProcess(DrClusterPtr cluster, DrString name, DrString commandLine,
              DrProcessTemplatePtr processTemplate);

    void SetAffinityList(DrAffinityListPtr list);
    DrAffinityListPtr GetAffinityList();
    DrProcessInfoPtr GetInfo();
    DrString GetName();

    void Schedule();
    void RequestProperty(UINT64 lastSeenVersion, DrString propertyName, DrPropertyListenerPtr listener);
    void SendCommand(DrString propertyName, DrString propertyDescription, DrByteArrayPtr propertyBlock);
    void Terminate();

    /* DrPSRListener implementation */
    virtual void ReceiveMessage(DrProcessStateRecordRef message);

    /* DrErrorListener implementation, used for the result of sending a command */
    virtual void ReceiveMessage(DrErrorRef message);

    /* DrPStateListener implementation, used to send a delayed request for termination */
    virtual void ReceiveMessage(DrProcessState message);

private:
    void CloneAndDeliverNotification(bool delay);

    DrClusterRef           m_cluster;
    DrString               m_name;
    DrString               m_commandLine;
    DrProcessTemplateRef   m_template;
    DrAffinityListRef      m_affinity;

    bool                   m_hasEverRequestedProperty;
    DrProcessInfoRef       m_info;
};

typedef DrSet<DrProcessRef> DrProcessSet;
DRREF(DrProcessSet);

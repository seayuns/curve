/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 2021-05-19
 * Author: chenwei
 */

#include "curvefs/src/mds/mds.h"

#include <brpc/channel.h>
#include <brpc/server.h>
#include <glog/logging.h>

#include <utility>

#include "curvefs/src/mds/mds_service.h"

namespace curvefs {
namespace mds {

using ::curve::election::LeaderElection;
using ::curve::idgenerator::EtcdIdGenerator;
using ::curve::kvstorage::EtcdClientImp;

Mds::Mds()
    : conf_(),
      inited_(false),
      running_(false),
      fsManager_(),
      fsStorage_(),
      spaceClient_(),
      metaserverClient_(),
      options_(),
      etcdClientInited_(false),
      etcdClient_(),
      leaderElection_(),
      status_(),
      etcdEndpoint_() {}

Mds::~Mds() {}

void Mds::InitOptions(std::shared_ptr<Configuration> conf) {
    conf_ = std::move(conf);
    conf_->GetValueFatalIfFail("mds.listen.addr", &options_.mdsListenAddr);
    conf_->GetValueFatalIfFail("mds.dummy.port", &options_.dummyPort);
    conf_->GetValueFatalIfFail("space.addr", &options_.spaceOptions.spaceAddr);
    conf_->GetValueFatalIfFail("space.rpcTimeoutMs",
                               &options_.spaceOptions.rpcTimeoutMs);
    conf_->GetValueFatalIfFail("metaserver.addr",
                               &options_.metaserverOptions.metaserverAddr);
    conf_->GetValueFatalIfFail("metaserver.rpcTimeoutMs",
                               &options_.metaserverOptions.rpcTimeoutMs);
}

void Mds::Init() {
    LOG(INFO) << "Init MDS start";

    InitEtcdClient();

    fsStorage_ = std::make_shared<PersisKVStorage>(etcdClient_);
    spaceClient_ = std::make_shared<SpaceClient>(options_.spaceOptions);
    metaserverClient_ =
        std::make_shared<MetaserverClient>(options_.metaserverOptions);
    fsManager_ = std::make_shared<FsManager>(fsStorage_, spaceClient_,
                                             metaserverClient_);
    LOG_IF(FATAL, !fsManager_->Init()) << "fsManager Init fail";

    chunkIdAllocator_ = std::make_shared<ChunkIdAllocatorImpl>(etcdClient_);

    inited_ = true;

    LOG(INFO) << "Init MDS success";
}

void Mds::Run() {
    LOG(INFO) << "Run MDS";
    if (!inited_) {
        LOG(ERROR) << "MDS not inited yet!";
        return;
    }

    brpc::Server server;
    // add heartbeat service
    MdsServiceImpl mdsService(fsManager_, chunkIdAllocator_);
    LOG_IF(FATAL,
           server.AddService(&mdsService, brpc::SERVER_DOESNT_OWN_SERVICE) != 0)
        << "add mdsService error";

    // start rpc server
    brpc::ServerOptions option;
    LOG_IF(FATAL, server.Start(options_.mdsListenAddr.c_str(), &option) != 0)
        << "start brpc server error";
    running_ = true;

    // To achieve the graceful exit of SIGTERM, you need to specify parameters
    // when starting the process: --graceful_quit_on_sigterm
    server.RunUntilAskedToQuit();
}

void Mds::Stop() {
    LOG(INFO) << "Stop MDS";
    if (!running_) {
        LOG(WARNING) << "Stop MDS, but MDS is not running, return OK";
        return;
    }
    brpc::AskToQuit();
    fsManager_->Uninit();
}

void Mds::StartDummyServer() {
    conf_->ExposeMetric("curvefs_mds");
    status_.expose("curvefs_mds_status");
    status_.set_value("follower");

    LOG_IF(FATAL, 0 != brpc::StartDummyServerAt(options_.dummyPort))
        << "Start dummy server failed";
}

void Mds::StartCompaginLeader() {
    InitEtcdClient();

    LeaderElectionOptions electionOption;
    InitLeaderElectionOption(&electionOption);
    electionOption.etcdCli = etcdClient_;
    electionOption.campaginPrefix = "";

    InitLeaderElection(electionOption);

    while (0 != leaderElection_->CampaginLeader()) {
        LOG(INFO) << leaderElection_->GetLeaderName()
                  << " compagin for leader again";
    }

    LOG(INFO) << "Compagin leader success, I am leader now";
    status_.set_value("leader");
    leaderElection_->StartObserverLeader();
}

void Mds::InitEtcdClient() {
    if (etcdClientInited_) {
        return;
    }

    EtcdConf etcdConf;
    InitEtcdConf(&etcdConf);

    int etcdTimeout = 0;
    int etcdRetryTimes = 0;
    conf_->GetValueFatalIfFail("etcd.operation.timeoutMs", &etcdTimeout);
    conf_->GetValueFatalIfFail("etcd.retry.times", &etcdRetryTimes);

    etcdClient_ = std::make_shared<EtcdClientImp>();

    int r = etcdClient_->Init(etcdConf, etcdTimeout, etcdRetryTimes);
    LOG_IF(FATAL, r != EtcdErrCode::EtcdOK)
        << "Init etcd client error: " << r
        << ", etcd address: " << std::string(etcdConf.Endpoints, etcdConf.len)
        << ", etcdtimeout: " << etcdConf.DialTimeout
        << ", operation timeout: " << etcdTimeout
        << ", etcd retrytimes: " << etcdRetryTimes;

    LOG_IF(FATAL, !CheckEtcd()) << "Check etcd failed";

    LOG(INFO) << "Init etcd client succeeded, etcd address: "
              << std::string(etcdConf.Endpoints, etcdConf.len)
              << ", etcdtimeout: " << etcdConf.DialTimeout
              << ", operation timeout: " << etcdTimeout
              << ", etcd retrytimes: " << etcdRetryTimes;

    etcdClientInited_ = true;
}

void Mds::InitEtcdConf(EtcdConf* etcdConf) {
    conf_->GetValueFatalIfFail("etcd.endpoint", &etcdEndpoint_);
    conf_->GetValueFatalIfFail("etcd.dailtimeoutMs", &etcdConf->DialTimeout);

    LOG(INFO) << "etcd.endpoint: " << etcdEndpoint_;
    LOG(INFO) << "etcd.dailtimeoutMs: " << etcdConf->DialTimeout;

    etcdConf->len = etcdEndpoint_.size();
    etcdConf->Endpoints = &etcdEndpoint_[0];
}

bool Mds::CheckEtcd() {
    std::string out;
    int r = etcdClient_->Get("test", &out);

    if (r != EtcdErrCode::EtcdOK && r != EtcdErrCode::EtcdKeyNotExist) {
        LOG(ERROR) << "Check etcd error: " << r;
        return false;
    } else {
        LOG(INFO) << "Check etcd ok";
        return true;
    }
}

void Mds::InitLeaderElectionOption(LeaderElectionOptions* option) {
    conf_->GetValueFatalIfFail("mds.listen.addr", &option->leaderUniqueName);
    conf_->GetValueFatalIfFail("leader.sessionInterSec",
                               &option->sessionInterSec);
    conf_->GetValueFatalIfFail("leader.electionTimeoutMs",
                               &option->electionTimeoutMs);
}

void Mds::InitLeaderElection(const LeaderElectionOptions& option) {
    leaderElection_ = std::make_shared<LeaderElection>(option);
}

}  // namespace mds
}  // namespace curvefs
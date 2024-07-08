//
// Created by jun on 24-6-23.
//

#include "photon/hook/ZySokect.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>
#include <arpa/inet.h>
#include <unistd.h>



/*********XyProtocol begin********/
namespace ZyIo::protocol
{
    XyProtocol::XyProtocol() = default;

    void XyProtocol::onMsg(char* buf, size_t len)
    {
        auto msg = std::string(buf, len);
        //copy buf to cache
        this->cacheMsg.append(msg);
        //decode
        decodeMsg();
    }

    void XyProtocol::decodeMsg()
    {
        auto startIdx = cacheMsg.find_first_of(startFlag);
        if (std::string::npos == startIdx)
        {
            //丢弃 msg
            cacheMsg.clear();
            return;
        }
        cacheMsg = cacheMsg.substr(startIdx);
        //min len: starFlag + msgLen + msg+ endFlag
        auto minLen = 1 + sizeof(size_t) + 1 + 1;
        if (cacheMsg.length() < minLen)
        {
            //等待下次解码
            return;
        }
        //msg len
        size_t msgLen;
        auto temp = cacheMsg.substr(1);
        memcpy(&msgLen, temp.c_str(), sizeof(size_t));
        std::cout << "msg len:" << msgLen << std::endl;
        //full msg len;
        size_t fullMsgLen = 1 + sizeof(size_t) + msgLen + 1;
        if (cacheMsg.length() < fullMsgLen)
        {
            //等待下次解码
            return;
        }
        char msgEndFlag = cacheMsg.at(fullMsgLen - 1);
        if (msgEndFlag != endFlag)
        {
            //丢弃 msg
            cacheMsg.clear();
            return;
        }
        auto fullMsg = cacheMsg.substr(0, fullMsgLen);
        onFullMsg(fullMsg.substr(1 + sizeof(size_t), msgLen));
        //move ptr
        this->cacheMsg = cacheMsg.substr(fullMsgLen);
        decodeMsg();
    }

    void XyProtocol::onFullMsg(std::string msg)
    {
        std::cout << "msg content:" << msg << std::endl;
    }

    std::string XyProtocol::encodeMsg(char* msg, size_t msgLen)
    {
        auto fullMsgLen = 1 + sizeof(msgLen) + msgLen + 1;
        char* m = (char*)malloc(sizeof(char) * fullMsgLen);
        memcpy(m, &startFlag, 1);
        m++;
        memcpy(m, &msgLen, sizeof(msgLen));
        m += sizeof(msgLen);
        memcpy(m, msg, msgLen);
        m += msgLen;
        memcpy(m, &endFlag, 1);
        m -= (fullMsgLen - 1);
        auto encode = std::string(m, fullMsgLen);
        free(m);
        return encode;
    }
}


/*********XyProtocol end********/


/*********ConnectData begin********/
ZyIo::ConnectData::ConnectData()
{
    this->sockPeer = new sockaddr_in;
    this->sockPeerLen = sizeof(sockaddr_in);
}

ZyIo::ConnectData::~ConnectData()
{
    delete sockPeer;
}

/*********ConnectData end********/


/*********AcceptData begin********/
ZyIo::AcceptData::AcceptData()
{
    this->sockPeerLen = new socklen_t;
    this->sockPeer = new sockaddr_in;
    *this->sockPeerLen = sizeof(sockaddr_in);
}

ZyIo::AcceptData::~AcceptData()
{
    delete sockPeerLen;
    delete sockPeer;
}

/*********AcceptData end********/

/*********SocketSession begin********/
ZyIo::SocketSession::SocketSession() = default;

ZyIo::SocketSession::SocketSession(RunType runType, int rwFd, sockaddr_in* sockaddrIn):
    runType(runType), rwFd(rwFd), sockPeer(sockaddrIn)
{
}

ZyIo::SocketSession::~SocketSession()
{
    std::cout << "SocketSession 销毁" << std::endl;
    close(rwFd);
}

void ZyIo::SocketSession::doWriteAsync(ZySokect* sokect, char* data, size_t len)
{
    sokect->submitWrite(shared_from_this(), data, len);
}


/*********SocketSession end********/


/*********RwData begin********/
unsigned int ZyIo::RwData::RW_DATE_LEN = 1024;

ZyIo::RwData::RwData() = default;

ZyIo::RwData::RwData(std::shared_ptr<SocketSession> session): session(std::move(session))
{
    this->len = RW_DATE_LEN;
    //malloc buf
    this->buf = (char*)malloc(sizeof(char) * len);
}

ZyIo::RwData::RwData(std::shared_ptr<SocketSession> session, char* data, size_t len): session(std::move(session)),
    len(len)
{
    //malloc buf
    this->buf = (char*)malloc(sizeof(char) * len);
    //copy data
    memcpy(buf, data, len);
}

ZyIo::RwData::~RwData()
{
    free(buf);
}

/*********RwData end********/


/*********DataCarrier begin********/
/**
 *free resoucre
 */
void ZyIo::DataCarrier::destroyResources(DataCarrier* carrier)
{
    auto flag = carrier->flag;
    if (flag == CONNETC)
    {
        auto data = static_cast<ConnectData*>(carrier->data);
        delete data;
        delete carrier;
        return;
    }
    if (flag == ACCEPT)
    {
        auto data = static_cast<AcceptData*>(carrier->data);
        delete data;
        delete carrier;
    }

    if (flag == READ || flag == WRITE)
    {
        auto data = static_cast<RwData*>(carrier->data);
        delete data;
        delete carrier;
    }
}

ZyIo::DataCarrier::DataCarrier() = default;

ZyIo::DataCarrier::DataCarrier(DataFlag flag, void* data): flag(flag), data(data)
{
}

ZyIo::DataCarrier::~DataCarrier() = default;

/*********DataCarrier end********/


/*********SessionManager begin********/
ZyIo::SessionManager::SessionManager() = default;

ZyIo::SessionManager::SessionManager(genSessionFun genSessionFun, distroySessionFun distroySessionFun):
    _genSession(genSessionFun), _distroySession(distroySessionFun)
{
    container = new std::unordered_map<int, std::shared_ptr<SocketSession>>;
}

ZyIo::SessionManager::~SessionManager()
{
    for (const auto& par : *container)
    {
        auto session = par.second;
        session.reset();
    }
    delete container;
}


std::shared_ptr<ZyIo::SocketSession> ZyIo::SessionManager::findSession(int rwFd)
{
    auto it = container->find(rwFd);
    if (it == container->end())
    {
        fprintf(stderr, "session not existed,rwFd: %d \n", rwFd);
        return nullptr;
    }
    return it->second;
}


std::shared_ptr<ZyIo::SocketSession> ZyIo::SessionManager::newSession2Container(
    RunType type, int rwFd, sockaddr_in* sockaddr_in)
{
    auto session = _genSession(type, rwFd, sockaddr_in);
    container->insert({session->getRwFd(), session});
    return session;
}

void ZyIo::SessionManager::distroySession(std::shared_ptr<SocketSession> session)
{
    auto rwFd = session->getRwFd();
    container->erase(rwFd);
    _distroySession(session);
}

/*********SessionManager end********/

/*********ZySokect begin********/
unsigned int ZyIo::ZySokect::DEFAULT_ENTITY_SIZE_S = 1024;
unsigned int ZyIo::ZySokect::DEFAULT_ENTITY_SIZE_C = 8;

ZyIo::ZySokect::ZySokect() = default;

ZyIo::ZySokect::ZySokect(unsigned int entitySize, SessionManager* sessionManager): sessionManager(sessionManager)
{
    auto ring = new io_uring;
    auto const ret = io_uring_queue_init(entitySize, ring, 0); // 初始化
    if (ret < 0)
    {
        fprintf(stderr, "io_uring_queue_init error: %s\n", strerror(-ret));
        exit(EXIT_FAILURE);
    }
    this->ring = ring;
}

ZyIo::ZySokect::~ZySokect()
{
    delete ring;
}

void ZyIo::ZySokect::submitRead(std::shared_ptr<SocketSession> session)
{
    auto data = new RwData(session);
    auto carrier = new DataCarrier(READ, data);
    auto sqe = doTake();
    //On files that are not capable of seeking, the offset must be 0 or -1.
    io_uring_prep_read(sqe, session->getRwFd(), data->getBuf(), data->getLen(), 0);
    doSubmit(sqe, carrier);
}

void ZyIo::ZySokect::submitWrite(std::shared_ptr<SocketSession> session, char* buf, size_t len)
{
    auto data = new RwData(session, buf, len);
    auto carrier = new DataCarrier(WRITE, data);
    auto sqe = doTake();
    //On files that are not capable of seeking, the offset must be 0 or -1.
    io_uring_prep_write(sqe, session->getRwFd(), data->getBuf(), data->getLen(), 0);
    doSubmit(sqe, carrier);
}

io_uring_sqe* ZyIo::ZySokect::doTake()
{
    return io_uring_get_sqe(ring);
}

void ZyIo::ZySokect::doSubmit(io_uring_sqe* sqe, DataCarrier* carrier)
{
    io_uring_sqe_set_data(sqe, carrier);
    io_uring_submit(ring);
}


void ZyIo::ZySokect::rwCqeHandle(io_uring_cqe* cqe)
{
    //READ == type || WRITE == type
    auto dataCarrier = (DataCarrier*)cqe->user_data;
    auto type = dataCarrier->getFlag();
    auto rwData = static_cast<RwData*>(dataCarrier->getData());
    if (!rwData->getSession())
    {
        fprintf(stderr, "session is destroy rw fd:x.\n");
        DataCarrier::destroyResources(dataCarrier);
        io_uring_cqe_seen(ring, cqe);
        return;
    }
    if (READ == type)
    {
        if (cqe->res <= 0)
        {
            fprintf(stderr, "socket  read error,code %d\n", cqe->res);
            onReadFaild(rwData->getSession());
            DataCarrier::destroyResources(dataCarrier);
            io_uring_cqe_seen(ring, cqe);
            return;
        }
        rwData->setLen(cqe->res);
        onReadSuccess(rwData->getSession(), rwData->getBuf(), rwData->getLen());
        DataCarrier::destroyResources(dataCarrier);
        io_uring_cqe_seen(ring, cqe);
        return;
    }
    //WRITE
    if (cqe->res < 0)
    {
        fprintf(stderr, "socket  write error,code %d\n", cqe->res);
        onWriteFaild(rwData->getSession(), rwData->getBuf(), rwData->getLen());
        DataCarrier::destroyResources(dataCarrier);
        io_uring_cqe_seen(ring, cqe);
        return;
    }
    onWriteSuccess(rwData->getSession());
    DataCarrier::destroyResources(dataCarrier);
    io_uring_cqe_seen(ring, cqe);
}


void ZyIo::ZySokect::onReadSuccess(std::shared_ptr<SocketSession> session, char* buf, size_t len)
{
    auto cmd = session->getOnReadSuccessFun()(this, session, buf, len);
    if (cmd == CloseFd)
    {
        sessionManager->distroySession(session);
        return;
    }
    if (cmd == KeepRead)
    {
        submitRead(session);
        return;
    }
    printf("onReadSuccess unsupport cmd %d in read action\r\n", cmd);
}

void ZyIo::ZySokect::onReadFaild(std::shared_ptr<SocketSession> session)
{
    auto cmd = session->getOnReadFaildFun()();
    if (cmd == CloseFd)
    {
        sessionManager->distroySession(session);
        return;
    }
    if (cmd == KeepRead)
    {
        submitRead(session);
        return;
    }
    printf("onReadFaild unsupport cmd %d in read action\r\n", cmd);
}


void ZyIo::ZySokect::onWriteSuccess(std::shared_ptr<SocketSession> session)
{
    auto cmd = session->getOnWriteSuccessFun()();
    if (cmd == CloseFd)
    {
        sessionManager->distroySession(session);
        return;
    }
    if (cmd == WriteFinish)
    {
        return;
    }
    printf("onWriteSuccess unsupport cmd %d in write action\r\n", cmd);
}

void ZyIo::ZySokect::onWriteFaild(std::shared_ptr<SocketSession> session, char* buff, size_t len)
{
    auto cmd = session->getOnWriteFaildFun()(this, session, buff, len);
    if (cmd == CloseFd)
    {
        sessionManager->distroySession(session);
        return;
    }
    if (cmd == WriteFinish)
    {
        return;
    }
    printf("onWriteSuccess unsupport cmd %d in write action\r\n", cmd);
}


/*********ZySokect end********/


/*********ZyServer begin********/
std::string ZyIo::ZyServer::DEFAULT_ADDR = "0.0.0.0";
unsigned int ZyIo::ZyServer::DEFAULT_PORT = 8080;

ZyIo::ZyServer::ZyServer() = default;

ZyIo::ZyServer::ZyServer(SessionManager* sessionManager):
    ZyServer(DEFAULT_ADDR, DEFAULT_PORT, DEFAULT_ENTITY_SIZE_S, sessionManager)
{
}

ZyIo::ZyServer::ZyServer(unsigned int port, SessionManager* sessionManager): ZyServer(
    DEFAULT_ADDR, port, DEFAULT_ENTITY_SIZE_S, sessionManager)
{
}

ZyIo::ZyServer::ZyServer(std::string address, unsigned int port, SessionManager* sessionManager)
    : ZyServer(std::move(address), port, DEFAULT_ENTITY_SIZE_S, sessionManager)
{
}

ZyIo::ZyServer::ZyServer(std::string address, unsigned int port, unsigned int entitySize,
                         SessionManager* sessionManager)
    : ZySokect(entitySize, sessionManager), addr(std::move(address)), port(port)
{
}

ZyIo::ZyServer::~ZyServer() = default;


void ZyIo::ZyServer::submitAccept()
{
    auto data = new AcceptData();
    auto carrier = new DataCarrier(ACCEPT, data);
    auto sqe = doTake();
    io_uring_prep_accept(sqe, sokectFd, (sockaddr*)data->getSockPeer(), data->getSockPeerLen(), 0);
    doSubmit(sqe, carrier);
}

void ZyIo::ZyServer::onAcceptSuccess(DataCarrier* carrier)
{
    auto acceptData = static_cast<AcceptData*>(carrier->getData());
    int peerFd = acceptData->getPeerFd();
    std::cout << "new session:" << peerFd << std::endl;
    auto session = getSessionManager()->newSession2Container(Server, peerFd, acceptData->getSockPeer());
    submitRead(session);
}

void ZyIo::ZyServer::onAcceptFail()
{
}


void ZyIo::ZyServer::start()
{
    /* 配置 Server Sock 信息。*/
    sockaddr_in srv_sock_addr = {};
    memset(&srv_sock_addr, 0, sizeof(srv_sock_addr));
    srv_sock_addr.sin_family = AF_INET;
    srv_sock_addr.sin_addr.s_addr = inet_addr(addr.c_str());
    srv_sock_addr.sin_port = htons(port);
    /* 创建 Server Socket。*/

    if (-1 == (this->sokectFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)))
    {
        fprintf(stderr, "Create socket file descriptor ERROR. code: %d \n", errno);
        exit(EXIT_FAILURE);
    }
    /* 设置 Server Socket 选项。*/
    int optval = 1;
    if (setsockopt(sokectFd,
                   SOL_SOCKET, // 表示套接字选项的协议层。
                   SO_REUSEADDR, // 表示在绑定地址时允许重用本地地址。这样做的好处是，当服务器进程崩溃或被关闭时，可以更快地重新启动服务器，而不必等待一段时间来释放之前使用的套接字。
                   &optval,
                   sizeof(optval)) < 0)
    {
        fprintf(stderr, "Set socket options ERROR. code: %s \n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    /* 绑定 Socket 与 Sock Address 信息。*/
    if (-1 == bind(sokectFd, (struct sockaddr*)&srv_sock_addr, sizeof(srv_sock_addr)))
    {
        fprintf(stderr, "Bind socket ERROR. code: %s \n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    /* 开始监听 Client 发出的连接请求。*/
    if (-1 == listen(sokectFd,
                     32 //指定请求队列的最大长度，当队列满了之后，就不再接收请求。参数跟系统连接数无关，相当于设置一个瞬间能处理的阈值,跟三次握手有关
    ))
    {
        fprintf(stderr, "Listen socket ERROR. code: %s \n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    /* 永远接受 Client 的连接请求。*/
    auto ring = getRing();
    submitAccept();
    while (true)
    {
        //批量获取
        io_uring_cqe* cqes[64] = {};
        auto cqeCount = io_uring_peek_batch_cqe(ring, cqes, 64);
        if (cqeCount <= 0)
        {
            continue;
        }
        for (int i = 0; i < cqeCount; i++)
        {
            auto cqe = cqes[i];
            //doHandleCqe(cqe);
            auto dataCarrier = (DataCarrier*)cqe->user_data;
            auto type = dataCarrier->getFlag();
            if (ACCEPT == type)
            {
                if (cqe->res < 0)
                {
                    fprintf(stderr, "socket  accept error,code %d\n", cqe->res);
                    onAcceptFail();
                }
                else
                {
                    auto acceptData = static_cast<AcceptData*>(dataCarrier->getData());
                    acceptData->setPeerFd(cqe->res);
                    onAcceptSuccess(dataCarrier);
                }
                DataCarrier::destroyResources(dataCarrier);
                io_uring_cqe_seen(ring, cqe);
                submitAccept();
                continue;
            }
            //wr handle
            rwCqeHandle(cqe);
        }
    }
}

/*********ZyServer end********/


/*********ZyClient begin********/
ZyIo::ZyClient::ZyClient() = default;

ZyIo::ZyClient::ZyClient(std::string addr, unsigned int port, SessionManager* sessionManager)
    : ZyClient(std::move(addr), port, DEFAULT_ENTITY_SIZE_C, sessionManager)
{
}

ZyIo::ZyClient::ZyClient(std::string addr, unsigned int port, unsigned int entitySize, SessionManager* sessionManager)
    : ZySokect(entitySize, sessionManager), addr(std::move(addr)), port(port)
{
    /* 配置 client Sock 信息。*/
    sockaddr_in srv_sock_addr = {};
    memset(&srv_sock_addr, 0, sizeof(srv_sock_addr));
    srv_sock_addr.sin_family = AF_INET;
    srv_sock_addr.sin_addr.s_addr = inet_addr(this->addr.c_str());
    srv_sock_addr.sin_port = htons(this->port);
    /* 创建 client socket。*/
    // 创建套接字
    auto sokectFd = socket(AF_INET, SOCK_STREAM, 0);
    // 客户端连接到服务器
    // auto res= connect(sokectFd, (struct sockaddr *)&srv_sock_addr, sizeof(srv_sock_addr));
    // if(res != 0) {
    //     fprintf(stderr, "connect server error. code: %s \n", strerror(errno));
    //     exit(EXIT_FAILURE);
    // }
    this->sokectFd = sokectFd;
    this->remoteAddr = srv_sock_addr;
}

ZyIo::ZyClient::~ZyClient()
{
}

void ZyIo::ZyClient::submitConnect()
{
    auto data = new ConnectData();
    (*data->getSockPeer()) = remoteAddr;
    auto carrier = new DataCarrier(CONNETC, data);
    auto sqe = doTake();
    io_uring_prep_connect(sqe, sokectFd, (sockaddr*)data->getSockPeer(), data->getSockPeerLen());
    doSubmit(sqe, carrier);
}

void ZyIo::ZyClient::onConnectSuccess(DataCarrier* carrier)
{
    auto session = getSessionManager()->newSession2Container(Client, sokectFd, &remoteAddr);
    submitRead(session);
    ready = true;
}


void ZyIo::ZyClient::onConnectFail()
{
}


void ZyIo::ZyClient::writeAsync(char* buf, size_t len)
{
    if (ready == true)
    {
        auto session = getSessionManager()->findSession(sokectFd);
        auto temp = session->getXyProtocol()->encodeMsg(buf, len);
        submitWrite(session, (char*)temp.c_str(), temp.length());
    }
}


void ZyIo::ZyClient::start()
{
    auto ring = getRing();
    submitConnect();
    while (true)
    {
        //批量获取
        io_uring_cqe* cqes[64] = {};
        auto cqeCount = io_uring_peek_batch_cqe(ring, cqes, 64);
        if (cqeCount <= 0)
        {
            continue;
        }
        for (int i = 0; i < cqeCount; i++)
        {
            auto cqe = cqes[i];
            //doHandleCqe(cqe);
            auto dataCarrier = (DataCarrier*)cqe->user_data;
            auto type = dataCarrier->getFlag();
            if (CONNETC == type)
            {
                if (cqe->res < 0)
                {
                    fprintf(stderr, "socket  connect error,code %d\n", cqe->res);
                    onConnectFail();
                    DataCarrier::destroyResources(dataCarrier);
                    io_uring_cqe_seen(ring, cqe);
                    break;
                }
                onConnectSuccess(dataCarrier);
                DataCarrier::destroyResources(dataCarrier);
                io_uring_cqe_seen(ring, cqe);
                continue;
            }
            //wr handle
            rwCqeHandle(cqe);
        }
    }
}

/*********ZyClient end********/


/******** DataCarrierV2 begin********/
ZyIo::DataCarrierV2::DataCarrierV2(DataFlag flag, int tid, __s32* res): flag(flag), tid(tid), res(res)
{
}

/******** DataCarrierV2 end********/

/*********ZySokectHook begin********/
ZyIo::ZySokectHook::ZySokectHook()
{
    auto pUring = new io_uring;
    auto const ret = io_uring_queue_init(ZyIo::ZySokect::DEFAULT_ENTITY_SIZE_S, pUring, 0); // 初始化
    if (ret < 0)
    {
        fprintf(stderr, "io_uring_queue_init error: %s\n", strerror(-ret));
        exit(EXIT_FAILURE);
    }
    this->ring = pUring;
}

ZyIo::ZySokectHook::~ZySokectHook()
{
    delete ring;
}

io_uring_sqe* ZyIo::ZySokectHook::doTake()
{
    return io_uring_get_sqe(ring);
}

void ZyIo::ZySokectHook::doSubmit(io_uring_sqe* sqe, DataCarrierV2* carrier)
{
    io_uring_sqe_set_data(sqe, carrier);
    io_uring_submit(ring);
}

void ZyIo::ZySokectHook::submitAccept(int tid, __s32* res, int sockfd, struct sockaddr* addr, socklen_t* addrlen)
{
    auto carrier = new DataCarrierV2(ACCEPT, tid, res);
    auto sqe = doTake();
    io_uring_prep_accept(sqe, sockfd, addr, addrlen, 0);
    doSubmit(sqe, carrier);
}

void ZyIo::ZySokectHook::submitConnect(int tid, __s32* res, int sockfd, const struct sockaddr* addr,
                                       socklen_t addrlen)
{
    auto carrier = new DataCarrierV2(CONNETC, tid, res);
    auto sqe = doTake();
    io_uring_prep_connect(sqe, sockfd, addr, addrlen);
    doSubmit(sqe, carrier);
}

void ZyIo::ZySokectHook::submitRead(int tid, __s32* res, int fd, void* buf, size_t count)
{
    auto carrier = new DataCarrierV2(READ, tid, res);
    auto sqe = doTake();
    io_uring_prep_read(sqe, fd, buf, count, 0);
    doSubmit(sqe, carrier);
}

void ZyIo::ZySokectHook::submitWrite(int tid, __s32* res, int fd, const void* buf, size_t count)
{
    auto carrier = new DataCarrierV2(READ, tid, res);
    auto sqe = doTake();
    io_uring_prep_write(sqe, fd, buf, count, 0);
    doSubmit(sqe, carrier);
}

void ZyIo::ZySokectHook::start()
{
    new photon_std::thread(&ZyIo::ZySokectHook::doStart, this);
}

void ZyIo::ZySokectHook::doStart()
{
    auto ring = getRing();
    while (true)
    {
        //批量获取
        io_uring_cqe* cqes[64] = {};
        auto cqeCount = io_uring_peek_batch_cqe(ring, cqes, 64);
        if (cqeCount <= 0)
        {
            continue;
        }
        for (int i = 0; i < cqeCount; i++)
        {
            auto cqe = cqes[i];
            auto dataCarrier = (DataCarrierV2*)cqe->user_data;
            //return res
            *(dataCarrier->getRes()) = cqe->res;
            //resume thead
            dataCarrier->getTid();


            photon::thread_interrupt(photon::thread* th, int error_number = EINTR);

            delete dataCarrier;
            io_uring_cqe_seen(ring, cqe);
        }
    }
}

/*********ZySokectHook end********/

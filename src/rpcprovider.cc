#include "rpcprovider.hpp"
#include "mprpcapplication.hpp"
#include "rpcheader.pb.h"
#include "zookeeperutil.hpp"

/*
service_name --> service描述
                    --> service* 记录已服务对象
                    method_name --> method 方法对象
*/

void RpcProvider::onConnection(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn->connected())
    {
        // 和rpc client的连接断开了
        conn->shutdown();
    }
}

/*
在框架内部，rpcprovider和rpcconsumer需要沟通好需要哪种protobuf的数据类型
16service_name method_name args  定义proto的message类型进行数据的序列化和反序列化
                                    service_name method_name args_size

header_size(4个字节) + header_str + args_str
*/
void RpcProvider::onMessage(const muduo::net::TcpConnectionPtr &conn,
                            muduo::net::Buffer *buffer,
                            muduo::Timestamp time)
{
    // 网络上接收的远程rpc调用请求的字符流 Login args
    std::string recv_buf = buffer->retrieveAllAsString();

    // 从字符流中读取前四个字节的内容
    uint32_t header_size = 0;
    recv_buf.copy((char *)&header_size, 4, 0);

    // 根绝header_size读取数据头的原始字符流,反序列化数据，得到rpc请求的详细信息
    std::string rpc_header_str = recv_buf.substr(4, header_size);
    mprpc::RpcHeader rpcHeader;
    std::string service_name;
    std::string method_name;
    uint32_t args_size;
    if (rpcHeader.ParseFromString(rpc_header_str))
    {
        // 数据头反序列化成功
        service_name = rpcHeader.service_name();
        method_name = rpcHeader.method_name();
        args_size = rpcHeader.args_size();
    }
    else
    {
        // 数据头反序列化失败
        std::cout << "rpc_header_str: " << rpc_header_str << " parse error!" << std::endl;
        return;
    }

    // 获取rpc方法参数的字符流数据
    std::string args_str = recv_buf.substr(4 + header_size, args_size);

    // 打印调试信息
    std::cout << "===================================" << std::endl;
    std::cout << "recv_buf: " << recv_buf << std::endl;
    std::cout << "header_size: " << header_size << std::endl;
    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
    std::cout << "service_name: " << service_name << std::endl;
    std::cout << "method_name: " << method_name << std::endl;
    std::cout << "args_str: " << args_str << std::endl;
    std::cout << "===================================" << std::endl;

    // 获取serice对象和method对象
    auto it = m_serviceInfoMap.find(service_name);
    if (it == m_serviceInfoMap.end())
    {
        std::cout << service_name << " is not existed" << std::endl;
        return;
    }
    google::protobuf::Service *service = it->second.m_service;

    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end())
    {
        std::cout << method_name << " is not existed" << std::endl;
    }
    const google::protobuf::MethodDescriptor *method = mit->second;

    // 生成rpc方法调用的请求request和响应response参数
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str))
    {
        std::cout << "request parse error! content: " << args_str << std::endl;
        return;
    }
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();

    // 给下面method方法的调用，绑定一个closure的回调函数
    google::protobuf::Closure *done = google::protobuf::NewCallback<RpcProvider,
                                                                    const muduo::net::TcpConnectionPtr &,
                                                                    google::protobuf::Message *>(this, &RpcProvider::SendRpcResponse, conn, response);
    // 在框架上根据远端rpc请求，调用本地的rpc方法
    service->CallMethod(method, nullptr, request, response, done);
}

// 这里是框架提供给外部使用的，可以发布rpc方法的函数接口
void RpcProvider::NotifyService(::google::protobuf::Service *service)
{
    ServiceInfo service_info;

    // 获取服务对象的描述
    const google::protobuf::ServiceDescriptor *psServiceDesc = service->GetDescriptor();

    // 获取服务的名字
    std::string service_name = psServiceDesc->name();

    // 获取服务的方法的数量
    int methondCnt = psServiceDesc->method_count();

    std::cout << "service name: " << service_name << std::endl;

    for (int i = 0; i < methondCnt; ++i)
    {
        // 获取服务对象执行下标的服务方法的描述(抽象描述)
        const google::protobuf::MethodDescriptor *pmethodDesc = psServiceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        service_info.m_methodMap.emplace(std::make_pair(method_name, pmethodDesc));

        std::cout << "method name: " << method_name << std::endl;
    }
    service_info.m_service = service;
    m_serviceInfoMap.emplace(std::make_pair(service_name, service_info));
}

// 启动rpc服务节点,开始提供rpc远程网络服务
void RpcProvider::Run()
{
    std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserver_ip");
    uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserver_port").c_str());
    muduo::net::InetAddress address(ip, port);

    // 创建Tcpserver对象
    muduo::net::TcpServer server(&m_eventloop, address, "RpcProvider");

    // 绑定连接回调消息和消息读写回调方法
    server.setConnectionCallback(std::bind(&RpcProvider::onConnection, this, std::placeholders::_1));
    server.setMessageCallback(std::bind(&RpcProvider::onMessage, this,
                                        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // 设置muduo库的线程数量
    server.setThreadNum(4);

    // 把当前rpc节点上要发布的服务全部注册到zk上面，让rpc client可以从zk上发现服务
    ZkClient zkCli;
    zkCli.Start();

    for (auto &sp : m_serviceInfoMap)
    {
        // /service_name
        std::string service_path = "/" + sp.first;
        zkCli.create(service_path.c_str(), nullptr, 0);
        for (auto &mp : sp.second.m_methodMap)
        {
            // /service_name/method_name
            std::string method_path = service_path + "/" + mp.first;
            char method_path_data[128] = {0};
            sprintf(method_path_data, "%s:%d", ip.c_str(), port);
            zkCli.create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
        }
    }

    // 启动网络服务
    server.start();
    m_eventloop.loop();
}

void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr &conn,
                                  google::protobuf::Message *response)
{
    std::string response_str;
    if (response->SerializeToString(&response_str))
    {
        // 序列化成功后，通过网络把rpc方法执行的结果发送回rpc的调用方
        conn->send(response_str);
    }
    else
    {
        std::cout << "serialize response_str error!" << std::endl;
    }
    conn->shutdown(); // 主动断开连接
}
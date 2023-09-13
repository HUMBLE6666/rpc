#include "mprpcchannel.hpp"
#include "mprpcapplication.hpp"
#include "mprpccontroller.hpp"
#include "zookeeperutil.hpp"
#include <string>
#include "rpcheader.pb.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/*
header_size + service_name + method_name + args_size + args
*/
// 所有通过stub对象调用的rpc方法，都走到这里，统一做数据的序列化和网络发送
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor *method,
                              google::protobuf::RpcController *controller,
                              const google::protobuf::Message *request,
                              google::protobuf::Message *response,
                              google::protobuf::Closure *done)
{
    const google::protobuf::ServiceDescriptor *sd = method->service();
    std::string service_name = sd->name();    // service_name
    std::string method_name = method->name(); // method_name

    // 获取参数的序列化字符串长度args_size
    uint32_t args_size = 0;
    std::string args_str;
    if (request->SerializeToString(&args_str))
    {
        args_size = args_str.size();
    }
    else
    {
        controller->SetFailed("Serialize request error!");
        return;
    }

    // 定义rpc的请求header
    mprpc::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size(args_size);

    uint32_t header_size;
    std::string rpc_header_str;
    if (rpcHeader.SerializeToString(&rpc_header_str))
    {
        header_size = rpc_header_str.size();
    }
    else
    {
        controller->SetFailed("Serialize rpc header error !");
        return;
    }

    // 组织待发送的rpc请求的字符串
    std::string send_rpc_str;
    send_rpc_str.insert(0, std::string((char *)&header_size, 4)); // header_size
    send_rpc_str += rpc_header_str;                               // rpc_header
    send_rpc_str += args_str;                                     // args

    // 打印调试信息
    std::cout << "===================================" << std::endl;
    std::cout << "header_size: " << header_size << std::endl;
    std::cout << "rpc_header_str: " << rpc_header_str << std::endl;
    std::cout << "service_name: " << service_name << std::endl;
    std::cout << "method_name: " << method_name << std::endl;
    std::cout << "args_str: " << args_str << std::endl;
    std::cout << "===================================" << std::endl;

    // 使用tcp编程，完成rpc方法的远程调用
    int cfd = socket(PF_INET, SOCK_STREAM, 0);
    if (-1 == cfd)
    {
        char errText[512] = {0};
        sprintf(errText, "create socket error! errno: %d", errno);
        controller->SetFailed(errText);
        return;
    }

    // std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserver_ip");
    // uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserver_port").c_str());
    ZkClient zkCli;
    zkCli.Start();
    std::string method_path = "/" + service_name + "/" + method_name;
    std::string host_data = zkCli.GetData(method_path.c_str());
    if (host_data == "")
    {
        controller->SetFailed(method_path + "is not exist!");
        return;
    }
    int idx = host_data.find(":");
    if (idx == -1)
    {
        controller->SetFailed(method_path + "address is invalid!");
        return;
    }
    std::string ip = host_data.substr(0, idx);
    uint16_t port = atoi(host_data.substr(idx + 1, host_data.size() - idx).c_str());

    struct sockaddr_in server;
    server.sin_family = PF_INET;
    server.sin_port = htons(port);
    inet_pton(PF_INET, ip.c_str(), &server.sin_addr.s_addr);

    // 连接rpc服务节点
    if (-1 == connect(cfd, (struct sockaddr *)&server, sizeof(server)))
    {
        close(cfd);
        char errText[512] = {0};
        sprintf(errText, "connect error! errno: %d", errno);
        controller->SetFailed(errText);
        return;
    }

    if (-1 == send(cfd, send_rpc_str.c_str(), send_rpc_str.size(), 0))
    {
        char errText[512] = {0};
        sprintf(errText, "send error! errno: %d", errno);
        controller->SetFailed(errText);
        close(cfd);
        return;
    }

    // 接收rpc请求的响应值
    char recv_buf[BUFSIZ] = {0};
    int recv_size = 0;
    if (-1 == (recv_size = recv(cfd, recv_buf, BUFSIZ, 0)))
    {
        char errText[512] = {0};
        sprintf(errText, "receive error! errno: %d", errno);
        controller->SetFailed(errText);
        close(cfd);
        return;
    }

    // std::string response_str(recv_buf, 0, recv_size); // 出现问题，buf中遇到/0就储存不了
    // if (!response->ParseFromString(response_str))
    if (!response->ParseFromArray(recv_buf, recv_size))
    {
        char errText[512] = {0};
        sprintf(errText, "parse error! errno: %s", recv_buf);
        controller->SetFailed(errText);
        close(cfd);
        return;
    }

    close(cfd);
}

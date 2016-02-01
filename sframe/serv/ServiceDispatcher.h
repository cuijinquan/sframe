﻿
#ifndef SFRAME_SERVICE_DISPATCHER_H
#define SFRAME_SERVICE_DISPATCHER_H

#include <assert.h>
#include <memory.h>
#include <memory>
#include <vector>
#include <unordered_set>
#include <thread>
#include "../util/RingQueue.h"
#include "../util/Singleton.h"
#include "../util/Serialization.h"
#include "Message.h"
#include "ProxyServiceMsg.h"

namespace sframe{

class IoService;
class Service;

// 服务调度器
class ServiceDispatcher : public singleton<ServiceDispatcher>, public noncopyable
{
public:
    static const int32_t kMaxServiceId = 1024; // 最大服务ID

private:

    // 工作线程函数
    static void ExecWorker(ServiceDispatcher * dispatcher);

public:
	ServiceDispatcher();

    ~ServiceDispatcher();

	// 发消息
	void SendMsg(int32_t sid, const std::shared_ptr<Message> & msg);

    // 发送消息
	template<typename T>
	void SendMsg(int32_t sid, const std::shared_ptr<T> & msg)
	{
		static_assert(std::is_base_of<Message, T>::value, "Message is not T Base");
		std::shared_ptr<Message> base_msg(msg);
		SendMsg(sid, base_msg);
	}

	// 发送内部服务消息
	template<typename... T_Args>
	void SendInsideServiceMsg(int32_t src_sid, int32_t dest_sid, uint16_t msg_id, T_Args&... args)
	{
		assert(dest_sid >= 0 && dest_sid <= kMaxServiceId);
		std::shared_ptr<InsideServiceMessage<T_Args...>> msg = std::make_shared<InsideServiceMessage<T_Args...>>(args...);
		msg->src_sid = src_sid;
		msg->dest_sid = dest_sid;
		msg->msg_id = msg_id;
		SendMsg(dest_sid, msg);
	}

	// 发送网络服务消息
	template<typename... T_Args>
	void SendNetServiceMsg(int32_t src_sid, int32_t dest_sid, uint16_t msg_id, T_Args&... args)
	{
		uint16_t msg_size = 0;
		int32_t total_streang_length = AutoGetSize(msg_size, src_sid, dest_sid, msg_id, args...);
		std::shared_ptr<std::vector<char>> data = std::make_shared<std::vector<char>>(total_streang_length, 0);
		char * buf = &((*data)[0]);

		StreamWriter writer(buf + sizeof(msg_size), total_streang_length - sizeof(msg_size));
		if (!AutoEncode(writer, src_sid, dest_sid, msg_id, args...))
		{
			assert(false);
			return;
		}

		msg_size = (uint16_t)writer.GetStreamLength();
		StreamWriter msg_size_writer(buf, sizeof(msg_size));
		if (!AutoEncode(msg_size_writer, msg_size))
		{
			assert(false);
			return;
		}

		std::shared_ptr<InsideServiceMessage<int32_t, std::shared_ptr<std::vector<char>>>> msg =
			std::make_shared<InsideServiceMessage<int32_t, std::shared_ptr<std::vector<char>>>>(dest_sid, data);
		msg->src_sid = src_sid;
		msg->dest_sid = 0;
		msg->msg_id = kProxyServiceMsgId_SendToRemoteService;

		SendMsg(0, msg);
	}

	// 发送服务消息
	template<typename... T_Args>
	void SendServiceMsg(int32_t src_sid, int32_t dest_sid, uint16_t msg_id, T_Args&... args)
	{
		assert(dest_sid >= 0 && dest_sid <= kMaxServiceId);

		if (_services[dest_sid])
		{
			SendInsideServiceMsg(src_sid, dest_sid, msg_id, args...);
		}
		else
		{
			SendNetServiceMsg(src_sid, dest_sid, msg_id, args...);
		}
	}

	// 获取IO服务
	const std::shared_ptr<IoService> & GetIoService() const
	{
		return _ioservice;
	}

	// 获取所有本地服务ID
	const std::vector<int32_t> & GetAllLocalSid() const
	{
		return _local_sid;
	}

	// 设置监听地址(供远程服务器连接的地址)
	void SetListenAddr(const std::string & ipv4, uint16_t port, const std::string & key = "");

    // 开始
    bool Start(int32_t thread_num);

    // 停止
    void Stop();

	// 调度服务(将指定服务压入调度队列)
	void Dispatch(int32_t sid);

	// 通知所有本地服务，其他服务的接入
	void NotifyServiceJoin(const std::unordered_set<int32_t> & service_set, bool is_remote);

    // 注册工作服务
    template<typename T>
    T * RegistService(int32_t sid)
    {
        assert(!_running);

		if (sid < 1 || sid > kMaxServiceId || _services[sid])
		{
			return nullptr;
		}

		T * s = new T();
		s->SetServiceId(sid);
		_max_sid = sid > _max_sid ? sid : _max_sid;
		_local_sid.push_back(sid);

		int32_t period = s->GetCyclePeriod();
		if (period > 0)
		{
			_cycle_timers.push_back(CycleTimer(sid, period));
		}

		_services[sid] = s;

        return s;
    }

	// 注册远程服务器
	bool RegistRemoteServer(const std::string & remote_ip, uint16_t remote_port, const std::string & remote_key = "");

private:
	// 准备代理服务
	void RepareProxyServer();

private:
    Service * _services[kMaxServiceId + 1];       // index:0为特殊的代理服务
    int32_t _max_sid;                             // 当前最大的服务ID
	std::vector<int32_t> _local_sid;              // 本地所有服务ID
    bool _running;                                // 是否正在运行
    std::vector<std::thread*> _threads;           // 所有线程
	std::shared_ptr<IoService> _ioservice;                       // IO服务指针
	RingQueue<int32_t, kMaxServiceId> _dispach_service_queue;   // 服务调度队列

	// 周期定时器
	struct CycleTimer
	{
		CycleTimer(int32_t sid, int32_t period) : sid(sid), next_time(0) 
		{
			msg = std::make_shared<CycleMessage>(period);
		}

		int32_t sid;                          // 服务ID
		int64_t next_time;                    // 下次执行时间
		std::shared_ptr<CycleMessage> msg;    // 周期消息
	};

	std::vector<CycleTimer> _cycle_timers;        // 周期定时器列表
	std::atomic_bool _checking_timer;             // 是否正在检查周期定时器
};

}

#endif
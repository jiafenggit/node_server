/*
 * Epoll_Watcher.cpp
 *
 *  Created on: Dec 16,2015
 *      Author: zhangyalei
 */

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include "Base_Function.h"
#include "Epoll_Watcher.h"

Epoll_Watcher::Epoll_Watcher(int type, int heart_second)
  : type_(type),
    end_flag_(false),
    epfd_(0),
    max_events_(512),
    events_(0),
    io_heart_idx_(0),
    heart_second_(heart_second) {
	//初始化epoll
	open();
}

Epoll_Watcher::~Epoll_Watcher(void) {}

int Epoll_Watcher::add(Event_Handler *evh, int op, Time_Value *tv) {
	if (evh == 0)
		return -1;

	if ((op & EVENT_TIMEOUT) && tv == 0) return -1;

	if ((op & EVENT_INPUT) || (op & EVENT_OUTPUT))
		add_io(evh, op);

	if (op & EVENT_TIMEOUT)
		add_timer(evh, op, tv);

	return 0;
}

int Epoll_Watcher::remove(Event_Handler *evh) {
	if (evh == 0) {
		LOG_TRACE("evh == 0");
		return -1;
	}

	if (((evh->get_io_flag() & EVENT_INPUT) || (evh->get_io_flag() & EVENT_OUTPUT)))
		remove_io(evh);

	if (evh->get_timer_flag() & EVENT_TIMEOUT)
		remove_timer(evh);

	return 0;
}

int Epoll_Watcher::loop(void) {
	while (true) {
		if (end_flag_)
			return -1;

		this->process_timer_event();
		this->watcher_loop();
	}
	return 0;
}

int Epoll_Watcher::end_loop(void) {
	GUARD(Mutex, mon, io_lock_);
	GUARD(Mutex, mon1, tq_lock_);
	end_flag_ = true;
	return 0;
}

int Epoll_Watcher::notify(void) {
	int ret = 0;
	if ((ret = ::write(pipe_fd_[1], "a", 1)) < 0) {
		LOG_ERROR("write pipe error");
	}
	return ret;
}

int Epoll_Watcher::open(void) {
	pending_io_map_.resize(max_fd(), (Event_Handler *)0);

	Heart_Map heart_map0(max_fd()), heart_map1(max_fd());
	io_heart_map_[0].swap(heart_map0);
	io_heart_map_[1].swap(heart_map1);

	//创建epoll
	if ((this->epfd_ = ::epoll_create(1)) == -1) {
		LOG_FATAL("epoll_create fatal");
		return -1;
	}

	timer_open();
	io_open();
	return 0;
}

int Epoll_Watcher::io_open(void) {
	if ((events_ = (struct epoll_event *)calloc(max_events_, sizeof(struct epoll_event))) == 0) {
		LOG_ERROR("calloc error");
		return -1;
	}

	//注册epoll类fd心跳定时器事件
	if (type_ & WITH_IO_HEARTBEAT) {
		Time_Value tv(heart_second_, 0);
		this->add(this, EVENT_TIMEOUT, &tv);
	}
	return 0;
}

int Epoll_Watcher::timer_open(void) {
	if (pipe(pipe_fd_) < 0) {
		LOG_ERROR("pipe error");
		return -1;
	}
	set_nonblock(pipe_fd_[0]);
	set_nonblock(pipe_fd_[1]);
	//将管道读端fd设置为epoll类fd
	this->set_fd(pipe_fd_[0]);
	//注册epoll类fd输入事件，当有线程往管道里面写数据时候，会触发该事件，使阻塞的epoll_wait返回，继续执行后面的定时器
	this->add(this, EVENT_INPUT);
	return 0;
}

int Epoll_Watcher::add_io(Event_Handler *evh, int op) {
	GUARD(Mutex, mon, io_lock_);
	if (end_flag_)
		return -1;

	if (evh->get_fd() == 0) {
		LOG_TRACE("fd == 0");
		return -1;
	}

	struct epoll_event ev;
	std::memset(&ev, 0, sizeof(ev));

	if (op & EVENT_INPUT) {
		evh->set_io_flag(EVENT_INPUT);
		ev.events |= EPOLLIN;
		if (op & EVENT_ONCE_IO_IN) {
			evh->set_io_flag(EVENT_ONCE_IO_IN);
			ev.events |= EPOLLONESHOT;
		}
	}
	if (op & EVENT_OUTPUT) {
		evh->set_io_flag(EVENT_OUTPUT);
		ev.events |= EPOLLOUT;
		if (op & EVENT_ONCE_IO_OUT)
			evh->set_io_flag(EVENT_ONCE_IO_OUT);
		ev.events |= EPOLLONESHOT;
	}
	if (ev.events)
		ev.events |= EPOLLET;

	ev.data.fd = evh->get_fd();

	pending_io_map_[evh->get_fd()] = evh;
	//非当前epoll类的fd监听io心跳事件
	if (evh->get_fd() != pipe_fd_[0] && (type_ & WITH_IO_HEARTBEAT)) {
		int heart_idx = next_heart_idx();
		evh->set_heart_idx(heart_idx);
		io_heart_map_[heart_idx].insert(std::make_pair(evh->get_fd(), evh));
	}

	if (::epoll_ctl(this->epfd_, EPOLL_CTL_ADD, evh->get_fd(), &ev) == -1) {
		LOG_ERROR("epoll_ctl error, fd:%d", evh->get_fd());
		pending_io_map_[evh->get_fd()] = 0;
		io_heart_map_[evh->get_heart_idx()].erase(evh->get_fd());
		return -1;
	}

	return 0;
}

int Epoll_Watcher::add_timer(Event_Handler *evh, int op, Time_Value *tv) {
	GUARD(Mutex, mon, tq_lock_);
	if (end_flag_)
		return -1;

	if (timer_set_.count(evh)) {
		LOG_TRACE("evh has in timer set, fd:", evh->get_fd());
		return -1;
	}

	if (op & EVENT_TIMEOUT) {
		evh->set_timer_flag(op);
		Time_Value absolute_tv(Time_Value::gettimeofday() + (*tv));
		evh->set_tv(absolute_tv, *tv);
		tq_.push(evh);
		timer_set_.insert(evh);
		//通知epoll有数据可读，使阻塞的epoll_wait返回，调用定时器处理函数
		notify();
	}

	return 0;
}

int Epoll_Watcher::remove_io(Event_Handler *evh) {
	GUARD(Mutex, mon, this->io_lock_);

	/// 删除IO事件
	int ret = 0;
	if ((ret = ::epoll_ctl(epfd_, EPOLL_CTL_DEL, evh->get_fd(), NULL)) == -1) {
		if (errno != EINTR)
			LOG_ERROR("epoll_ctl");
	}
	pending_io_map_[evh->get_fd()] = 0;

	/// 删除IO心跳
	if (type_ & WITH_IO_HEARTBEAT) {
		for (size_t i = 0; i < 2; ++i) {
			io_heart_map_[i].erase(evh->get_fd());
		}
	}

	return 0;
}

int Epoll_Watcher::remove_timer(Event_Handler *evh) {
	GUARD(Mutex, mon, tq_lock_);

	Event_Timer_Set::iterator timer_it = timer_set_.find(evh);
	if (timer_it == timer_set_.end()) // 防止定时器不在时间队列时的最差效率情况
		return 0;

	/// 删除定时事件
	Event_Handler *e = 0;
	std::vector<Event_Handler *> tvec;
	while (! tq_.empty()) {
		if ((e = tq_.top()) == evh) {
			tq_.pop();
			break;
		} else {
			tq_.pop();
			tvec.push_back(e);
		}
	}

	for (std::vector<Event_Handler *>::iterator it = tvec.begin(); it != tvec.end(); ++it) {
		tq_.push(*it);
	}

	timer_set_.erase(timer_it);
	return 0;
}

int Epoll_Watcher::calculate_timeout(void) {
	GUARD(Mutex, mon, tq_lock_);

	int timeout = -1;
	if(! tq_.empty()) {
		Event_Handler *evh = this->tq_.top();
		if (evh != 0) {
			Time_Value now(Time_Value::gettimeofday());

			if ((evh->get_absolute_tv()) <= now) {
				timeout = 0;
			} else {
				/**
				 * 此处ms_addition为补偿epoll最小单位使用毫秒，Time_Value最小单位使用微妙造成的误差
				 * 虽然这样处理理论上可能会令定时器出现1毫秒的误差，
				 * 但这样处理可以减少系统调用epoll_wait的调用次数，并简化代码，故在此采用。
				 */
				Time_Value in_tv(evh->get_absolute_tv() - now);
				int ms_addition = (in_tv.usec() % 1000) > 0 ? 1 : 0;

				double to = (in_tv.sec() * 1000) + (in_tv.usec() / 1000) + ms_addition;
				timeout = to;
			}
		}
	}

	return timeout;
}

void Epoll_Watcher::process_timer_event(void) {
	GUARD(Mutex, mon, tq_lock_);
	if (end_flag_)
		return ;

	Event_Handler *evh = 0;
	while (1) {
		if (tq_.empty())
			break;

		if ((evh = this->tq_.top()) == 0) {
			LOG_ERROR("evh is null");
			continue;
		}

		Time_Value now(Time_Value::gettimeofday());
		if (evh->get_absolute_tv() <= now) {
			this->tq_.pop();
			evh->handle_timeout(now);
			if (! (evh->get_timer_flag() & EVENT_ONCE_TIMER)) {
				now += evh->get_relative_tv();
				evh->set_tv(now);
				this->tq_.push(evh);
			} else {
				timer_set_.erase(evh);
			}
		} else {
			break;
		}
	}
}

void Epoll_Watcher::watcher_loop(void) {
	//epoll_wait最后一个参数含义：-1表示一直阻塞等待，0表示立即返回，大于0表示等待事件
	int nfds = ::epoll_wait(this->epfd_, this->events_, this->max_events_, this->calculate_timeout());
	if (nfds == -1) {
		if (errno != EINTR) {
			LOG_ERROR("epoll_wait error");
		}
		return ;
	}

	GUARD(Mutex, mon, io_lock_);
	if (end_flag_)
		return ;

	Event_Handler *evh = 0;
	for (int i = 0; i < nfds; ++i) {
		if ((evh = pending_io_map_[events_[i].data.fd]) != 0) {
			if (events_[i].events & EPOLLIN) {
				evh->handle_input();

				//添加evh到下次心跳监听map
				if ((type_ & WITH_IO_HEARTBEAT) && (events_[i].data.fd != pipe_fd_[0])
						&& (!(evh->get_io_flag() & EVENT_ONCE_IO_IN)) && (evh->get_heart_idx() != next_heart_idx())) {
					int heart_idx = next_heart_idx();
					io_heart_map_[evh->get_heart_idx()].erase(evh->get_fd());
					io_heart_map_[heart_idx].insert(std::make_pair(evh->get_fd(), evh));
					evh->set_heart_idx(heart_idx);
					//LOG_WARN("change evh, cur_heart_idx:%d next_heart_idx:%d fd:%d", io_heart_idx_, heart_idx, evh->get_fd());
				}
			}
		}
		if (events_[i].events & EPOLLOUT) {
			evh->handle_output();
		}

		if ((evh->get_io_flag() & EVENT_ONCE_IO_IN) || (evh->get_io_flag() & EVENT_ONCE_IO_OUT)) { /// 清除一次性IO事件
			pending_io_map_[events_[i].data.fd] = 0;
		}
	}
}

int Epoll_Watcher::handle_input(void) {
	int ret = 0;
	char tbuf[2048];

	while (1) {
		ret = ::read(pipe_fd_[0], tbuf, sizeof(tbuf));
		if (ret == 0) {
			LOG_ERROR("read pipe return EOF");
			break;
		}
		if (ret < 0) {
			if (errno == EWOULDBLOCK)
				break;
		}
	}

	return 0;
}

int Epoll_Watcher::handle_timeout(const Time_Value &tv) {
	GUARD(Mutex, mon, io_lock_);

	//epoll注册的io心跳定时器到期，关闭心跳连接
	std::vector<Event_Handler *> remove_vec;
	for (Heart_Map::iterator iter = io_heart_map_[io_heart_idx_].begin(); iter != io_heart_map_[io_heart_idx_].end(); ++iter) {
		remove_vec.push_back(iter->second);
	}
	for (std::vector<Event_Handler *>::iterator it = remove_vec.begin(); it != remove_vec.end(); ++it) {
		LOG_ERROR("epoll heart_timeout, cur_heart_idx:%d next_heart_idx:%d close fd:%d", io_heart_idx_, next_heart_idx(), (*it)->get_fd());
		(*it)->handle_close();
	}

	io_heart_map_[io_heart_idx_].clear();
	io_heart_idx_ = next_heart_idx();
	return 0;
}

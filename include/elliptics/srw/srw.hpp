#ifndef __SRW_HPP
#define __SRW_HPP

#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <iostream>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <sstream>

#include <boost/algorithm/string.hpp>

#include <elliptics/srw/worker.hpp>

namespace ioremap {
namespace srw {

typedef boost::shared_ptr<spawn> shared_proc_t;
class pool {
	public:
		pool(struct srw_init_ctl *ctl) : m_log(ctl->log, std::ios::app) {
			for (int i = 0; i < ctl->num; ++i) {
				shared_proc_t sp(new spawn(ctl));

				m_workers_idle.insert(std::make_pair(sp->pid(), sp));
			}

			/*
			 * std::fstream does not have filebuf with fd access anymore (at least with libstdc++ which comes with 3+ gcc)
			 * so we can not set O_CLOEXEC file like this
			 * 	fcntl(m_log.rdbuf()->fd(), F_GETFD, &flags);
			 * 	fcntl(m_log.rdbuf()->fd(), F_SETFD, flags | O_CLOEXEC);
			 * thus spawned workers will have a bunch of opened m_log files
			 */
		}

		virtual ~pool() {
		}

		void drop(int pid) {
			boost::mutex::scoped_lock workers_guard(m_workers_lock);
			shared_proc_t sp;

			m_workers.erase(pid);
			workers_guard.unlock();

			boost::mutex::scoped_lock apps_guard(m_apps_lock);
			for (std::map<std::string, std::vector<int> >::iterator it = m_apps.begin(); it != m_apps.end(); ++it) {
				for (std::vector<int>::iterator pid_it = it->second.begin(); pid_it != it->second.end(); ++pid_it) {
					if (*pid_it == pid) {
						it->second.erase(pid_it);
						break;
					}
				}
			}
		}

		std::string process(const struct sph &header, const char *data) {
			return select_worker(header, data);
		}

	private:
		std::ofstream m_log;
		boost::mutex m_workers_lock;
		boost::mutex m_apps_lock;
		boost::condition m_cond;
		std::map<int, shared_proc_t> m_workers_idle, m_workers;
		std::map<std::string, std::vector<int> > m_apps;

		std::string select_worker(const struct sph &header, const char *data) {
			std::string event = get_event(header, data);

			std::vector<std::string> strs;
			boost::split(strs, event, boost::is_any_of("/"));

			if (strs.size() != 2) {
				std::ostringstream str;
				str << event << ": event must be '$app/$event' or 'new-task/$name'";
				m_log << str.str() << std::endl;
				throw std::runtime_error(str.str());
			}

			std::string app = strs[0];

			if (strs[0] == "new-task") {
				new_task(header, data, strs[1]);
				app = strs[1];
			}

			boost::mutex::scoped_lock guard(m_apps_lock);
			std::map<std::string, std::vector<int> >::iterator it = m_apps.find(app);

			if (it == m_apps.end()) {
				std::ostringstream str;
				str << event << ": could not find handler";
				m_log << str.str() << std::endl;
				throw std::runtime_error(str.str());
			}

			std::vector<int> pids = it->second;
			guard.unlock();

			return worker_process(pids, header, data);
		}

		void new_task(const struct sph &header, const char *data, const std::string &name) {
			boost::mutex::scoped_lock guard(m_workers_lock);
			if ((int)m_workers_idle.size() < header.num) {
				std::ostringstream str;
				str << get_event(header, data) << ": can not get " << header.num << " idle workers, have only " <<
					m_workers_idle.size();
				m_log << str.str() << std::endl;
				throw std::runtime_error(str.str());
			}

			std::vector<int> pids;
			for (int i = 0; i < header.num; ++i) {
				shared_proc_t worker = m_workers_idle.begin()->second;

				pids.push_back(worker->pid());
				m_workers_idle.erase(m_workers_idle.begin());

				m_workers.insert(std::make_pair(worker->pid(), worker));
			}

			guard.unlock();

			boost::mutex::scoped_lock apps_guard(m_apps_lock);
			m_apps.insert(std::make_pair(name, pids));

			m_log << get_event(header, data) << ": created new task '" << name <<
				"', workers: " << pids.size() <<
				", pids: ";
			for (std::vector<int>::iterator it = pids.begin(); it != pids.end(); ++it) {
				m_log << *it << " ";
			}
			m_log << std::endl;
		}

		std::string worker_process(std::vector<int> &pids, const struct sph &header, const char *data) {
			std::string ret;
			while (pids.size()) {
				boost::mutex::scoped_lock guard(m_workers_lock);

				int pid_pos, pid;

				if (header.key == -1) {
					pid_pos = 0;
					pid = pids[0];
				} else {
					pid_pos = header.key % pids.size();
					pid = pids[pid_pos];
				}

				std::map<int, shared_proc_t>::iterator it = m_workers.find(pid);
				if (it == m_workers.end()) {
					std::ostringstream str;
					m_log << pid << ": " << get_event(header, data) << ": worker is dead";
					m_log << str.str() << std::endl;
					throw std::runtime_error(str.str());
				}

				guard.unlock();

				try {
					ret = it->second->process(header, data);

					m_log << getpid() << ": worker_process: " << get_event(header, data) << ": pid: " << pid <<
						", key: " << header.key <<
						", event-size: " << header.event_size <<
						", data-size: " << header.data_size <<
						", binary-size: " << header.binary_size <<
						", reply-size: " << ret.size() <<
						std::endl;

					if (header.key == -1) {
						pids.erase(pids.begin());
						continue;
					}

					break;
				} catch (const std::exception &e) {
					m_log << pid << ": " << get_event(header, data) << ": exception: " <<
						e.what() << std::endl;

					pids.erase(pids.begin() + pid_pos);

					if (pids.size() == 0)
						throw;
				}
			}

			return ret;
		}

		void register_worker(const std::string &event, const std::vector<int> pids) {
			boost::mutex::scoped_lock guard(m_apps_lock);

			std::pair<std::map<std::string, std::vector<int> >::iterator, bool> ret = m_apps.insert(std::make_pair(event, pids));
			if (ret.second == false) {
				m_apps.erase(ret.first);
				m_apps.insert(std::make_pair(event, pids));
			}
		}
};

} /* namespace srw */
} /* namespace ioremap */
#endif
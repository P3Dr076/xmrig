/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2016-2017 XMRig       <support@xmrig.com>
 *
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cmath>


#include "Console.h"
#include "interfaces/IJobResultListener.h"
#include "Mem.h"
#include "workers/DoubleWorker.h"
#include "workers/Handle.h"
#include "workers/SingleWorker.h"
#include "workers/Telemetry.h"
#include "workers/Workers.h"


IJobResultListener *Workers::m_listener = nullptr;
Job Workers::m_job;
std::atomic<int> Workers::m_paused;
std::atomic<uint64_t> Workers::m_sequence;
std::list<JobResult> Workers::m_queue;
std::vector<Handle*> Workers::m_workers;
Telemetry *Workers::m_telemetry = nullptr;
uint64_t Workers::m_ticks = 0;
uv_async_t Workers::m_async;
uv_mutex_t Workers::m_mutex;
uv_rwlock_t Workers::m_rwlock;
uv_timer_t Workers::m_timer;


Job Workers::job()
{
    uv_rwlock_rdlock(&m_rwlock);
    Job job = m_job;
    uv_rwlock_rdunlock(&m_rwlock);

    return job;
}


void Workers::setJob(const Job &job)
{
    uv_rwlock_wrlock(&m_rwlock);
    m_job = job;
    uv_rwlock_wrunlock(&m_rwlock);

    m_sequence++;
    m_paused = 0;
}


void Workers::start(int64_t affinity, bool nicehash)
{
    const int threads = Mem::threads();
    m_telemetry = new Telemetry(threads);

    uv_mutex_init(&m_mutex);
    uv_rwlock_init(&m_rwlock);

    m_sequence = 0;
    m_paused   = 1;

    uv_async_init(uv_default_loop(), &m_async, Workers::onResult);
    uv_timer_init(uv_default_loop(), &m_timer);
    uv_timer_start(&m_timer, Workers::onTick, 500, 500);

    for (int i = 0; i < threads; ++i) {
        Handle *handle = new Handle(i, threads, affinity, nicehash);
        m_workers.push_back(handle);
        handle->start(Workers::onReady);
    }
}


void Workers::submit(const JobResult &result)
{
    uv_mutex_lock(&m_mutex);
    m_queue.push_back(result);
    uv_mutex_unlock(&m_mutex);

    uv_async_send(&m_async);
}



void Workers::onReady(void *arg)
{
    auto handle = static_cast<Handle*>(arg);
    if (Mem::isDoubleHash()) {
        handle->setWorker(new DoubleWorker(handle));
    }
    else {
        handle->setWorker(new SingleWorker(handle));
    }

    handle->worker()->start();
}


void Workers::onResult(uv_async_t *handle)
{
    std::list<JobResult> results;

    uv_mutex_lock(&m_mutex);
    while (!m_queue.empty()) {
        results.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    uv_mutex_unlock(&m_mutex);

    for (auto result : results) {
        m_listener->onJobResult(result);
    }

    results.clear();
}


void Workers::onTick(uv_timer_t *handle)
{
    for (Handle *handle : m_workers) {
        if (handle->worker()) {
            m_telemetry->add(handle->threadId(), handle->worker()->hashCount(), handle->worker()->timestamp());
        }
    }

    if ((m_ticks++ & 0xF) == 0)  {
        double hps = 0.0;
        double telem;
        bool normal = true;

        for (Handle *handle : m_workers) {
            telem = m_telemetry->calc(handle->threadId(), 2500);
            if (!std::isnormal(telem)) {
                normal = false;
                break;
            }
            else {
                hps += telem;
            }
        }

        if (normal) {
            LOG_NOTICE("%03.1f H/s", hps);
        }
    }
}

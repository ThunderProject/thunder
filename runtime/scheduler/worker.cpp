module worker;

import cpu_scheduler;

thunder::cpu::worker::worker(thunder::cpu::scheduler &scheduler)
    : m_handle(scheduler)
{

}
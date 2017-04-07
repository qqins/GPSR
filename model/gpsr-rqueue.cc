/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "gpsr-rqueue.h"
#include <algorithm>
#include <functional>
#include "ns3/ipv4-route.h"
#include "ns3/socket.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE ("GpsrRequestQueue");

namespace ns3 {
namespace gpsr {
//返回queue的大小
uint32_t
RequestQueue::GetSize ()
{
  Purge ();//先清除过期的request
  return m_queue.size (); //返回queue大小
}

bool
RequestQueue::Enqueue (QueueEntry & entry)
{
  Purge ();//先清除过期的request


  //查找是否有相同的request，如果有了就返回false
  for (std::vector<QueueEntry>::const_iterator i = m_queue.begin (); i
       != m_queue.end (); ++i)
    {
      if ((i->GetPacket ()->GetUid () == entry.GetPacket ()->GetUid ())
          && (i->GetIpv4Header ().GetDestination ()
              == entry.GetIpv4Header ().GetDestination ()))
        {
          return false;
        }
    }
  //设置时间
  entry.SetExpireTime (m_queueTimeout);
  //超过长度，就将最前面的request删除
  if (m_queue.size () == m_maxLen)
    {
      Drop (m_queue.front (), "Drop the most aged packet");     // Drop the most aged packet
      m_queue.erase (m_queue.begin ());
    }
  m_queue.push_back (entry);
  return true;
}

//丢弃目标节点的发送请求
void
RequestQueue::DropPacketWithDst (Ipv4Address dst)
{
  NS_LOG_FUNCTION (this << dst);
  Purge ();
  //const Ipv4Address addr = dst;
  for (std::vector<QueueEntry>::iterator i = m_queue.begin (); i
       != m_queue.end (); ++i)
    {
      if (IsEqual (*i, dst))
        {
          Drop (*i, "DropPacketWithDst ");
        }
    }
  m_queue.erase (std::remove_if (m_queue.begin (), m_queue.end (),
                                 std::bind2nd (std::ptr_fun (RequestQueue::IsEqual), dst)), m_queue.end ());
                                 //remove_if 是将找到的元素删除，后面的元素前移一位，但向量的最后一位却还保留着之前的最后一位，所以用erase将end()的元素从queue中删除
}

//如果request发送的地址与目标节点相同就出队
bool
RequestQueue::Dequeue (Ipv4Address dst, QueueEntry & entry)
{
  Purge ();
  for (std::vector<QueueEntry>::iterator i = m_queue.begin (); i != m_queue.end (); ++i)
    {
      if (i->GetIpv4Header ().GetDestination () == dst)
        {
          entry = *i;
          m_queue.erase (i);
          return true;
        }
    }
  return false;
}

//寻找request中是否有目标节点的地址
bool
RequestQueue::Find (Ipv4Address dst)
{
  for (std::vector<QueueEntry>::const_iterator i = m_queue.begin (); i
       != m_queue.end (); ++i)
    {
      if (i->GetIpv4Header ().GetDestination () == dst)
        {
          return true;
        }
    }
  return false;
}


//IsExpired是内嵌类，有判断是否超时的方法，通过操作（）通过RequestQueue.IsExpired（QueueEntry）来访问
struct IsExpired
{
  bool
  operator() (QueueEntry const & e) const
  {
    return (e.GetExpireTime () < Seconds (0));
  }
};

void
RequestQueue::Purge ()
{
  IsExpired pred;
  for (std::vector<QueueEntry>::iterator i = m_queue.begin (); i
       != m_queue.end (); ++i)
    {
      if (pred (*i))
        {
          Drop (*i, "Drop outdated packet ");
        }
    }
  m_queue.erase (std::remove_if (m_queue.begin (), m_queue.end (), pred),
                 m_queue.end ());
}

void
RequestQueue::Drop (QueueEntry en, std::string reason)
{
  NS_LOG_LOGIC (reason << en.GetPacket ()->GetUid () << " " << en.GetIpv4Header ().GetDestination ());
  en.GetErrorCallback () (en.GetPacket (), en.GetIpv4Header (),
                          Socket::ERROR_NOROUTETOHOST);
  return;
}

}
}

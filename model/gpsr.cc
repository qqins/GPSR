/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * GPSR
 *
 */
#define NS_LOG_APPEND_CONTEXT                                           \
        if (m_ipv4) { std::clog << "[node " << m_ipv4->GetObject<Node> ()->GetId () << "] "; }

#include "gpsr.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/random-variable-stream.h"
#include "ns3/inet-socket-address.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/wifi-net-device.h"
#include "ns3/adhoc-wifi-mac.h"
#include <algorithm>
#include <limits>
#include <ns3/udp-header.h>
#include "ns3/seq-ts-header.h"


#define GPSR_LS_GOD 0

#define GPSR_LS_RLS 1

NS_LOG_COMPONENT_DEFINE ("GpsrRoutingProtocol");

namespace ns3 {
namespace gpsr {


//先定义了DeferedRouteOutputTag的类，包含一个m_isCallFromL3的标志，初始化为0
struct DeferredRouteOutputTag : public Tag
{
        /// Positive if output device is fixed in RouteOutput
        uint32_t m_isCallFromL3;

        DeferredRouteOutputTag () : Tag (),
                m_isCallFromL3 (0)
        {
        }

//TypeId
// TODO read ns3 api
        static TypeId GetTypeId ()
        {
                static TypeId tid = TypeId ("ns3::gpsr::DeferredRouteOutputTag").SetParent<Tag> (); //设置parent的tid,获取<Tag>是模版
                return tid;
        }

        TypeId  GetInstanceTypeId () const
        {
                return GetTypeId ();
        }

        uint32_t GetSerializedSize () const
        {
                return sizeof(uint32_t);
        }

        void  Serialize (TagBuffer i) const
        {
                i.WriteU32 (m_isCallFromL3);
        }

        void  Deserialize (TagBuffer i)
        {
                m_isCallFromL3 = i.ReadU32 ();
        }

        void  Print (std::ostream &os) const
        {
                os << "DeferredRouteOutputTag: m_isCallFromL3 = " << m_isCallFromL3;
        }
};

Ptr<UniformRandomVariable> x = CreateObject<UniformRandomVariable> ();

/********** Miscellaneous constants **********/

/// Maximum allowed jitter.
#define GPSR_MAXJITTER          (HelloInterval.GetSeconds () / 2)
/// Random number between [(-GPSR_MAXJITTER)-GPSR_MAXJITTER] used to jitter HELLO packet transmission.
#define JITTER (Seconds (x->GetValue (-GPSR_MAXJITTER, GPSR_MAXJITTER)))
#define FIRST_JITTER (Seconds (x->GetValue (0, GPSR_MAXJITTER))) //first Hello can not be in the past, used only on SetIpv4



NS_OBJECT_ENSURE_REGISTERED (RoutingProtocol);

/// UDP Port for GPSR control traffic, not defined by IANA yet
const uint32_t RoutingProtocol::GPSR_PORT = 666;

//构造函数，初始化；
RoutingProtocol::RoutingProtocol ()
        : HelloInterval (Seconds (1)),
        MaxQueueLen (64),
        MaxQueueTime (Seconds (30)),
        m_queue (MaxQueueLen, MaxQueueTime),
        HelloIntervalTimer (Timer::CANCEL_ON_DESTROY),
        PerimeterMode (false)
{
        m_neighbors = PositionTable ();
}


//增加TypeId作为唯一的interface接口，可以不适用对象来访问其属性值,或者调用trace
TypeId
RoutingProtocol::GetTypeId (void)
{
        static TypeId tid = TypeId ("ns3::gpsr::RoutingProtocol")
                            .SetParent<Ipv4RoutingProtocol> ()
                            .AddConstructor<RoutingProtocol> ()
                            .AddAttribute ("HelloInterval", "HELLO messages emission interval.",
                                           TimeValue (Seconds (1)),
                                           MakeTimeAccessor (&RoutingProtocol::HelloInterval),
                                           MakeTimeChecker ())
                            .AddAttribute ("LocationServiceName", "Indicates wich Location Service is enabled",
                                           EnumValue (GPSR_LS_GOD),
                                           MakeEnumAccessor (&RoutingProtocol::LocationServiceName),
                                           MakeEnumChecker (GPSR_LS_GOD, "GOD",
                                                            GPSR_LS_RLS, "RLS"))
                            .AddAttribute ("PerimeterMode", "Indicates if PerimeterMode is enabled",
                                           BooleanValue (false),
                                           MakeBooleanAccessor (&RoutingProtocol::PerimeterMode),
                                           MakeBooleanChecker ())
        ;
        return tid;
}

RoutingProtocol::~RoutingProtocol ()
{
}

void
RoutingProtocol::DoDispose ()
{
        m_ipv4 = 0;
        Ipv4RoutingProtocol::DoDispose ();
}

Ptr<LocationService>
RoutingProtocol::GetLS ()
{
        return m_locationService;
}
void
RoutingProtocol::SetLS (Ptr<LocationService> locationService)
{
        m_locationService = locationService;
}

//节点接受到了一个包，需要将包向前传
bool RoutingProtocol::RouteInput (Ptr<const Packet> p, const Ipv4Header &header, Ptr<const NetDevice> idev,
                                  UnicastForwardCallback ucb, MulticastForwardCallback mcb,
                                  LocalDeliverCallback lcb, ErrorCallback ecb)
{

        NS_LOG_FUNCTION (this << p->GetUid () << header.GetDestination () << idev->GetAddress ());
        NS_LOG_DEBUG("packet input"<<p->GetSize());
        if (m_socketAddresses.empty ())
        {
                NS_LOG_LOGIC ("No gpsr interfaces");
                return false;
        }
        NS_ASSERT (m_ipv4 != 0);
        NS_ASSERT (p != 0);
        // Check if input device supports IP
        NS_ASSERT (m_ipv4->GetInterfaceForDevice (idev) >= 0);
        int32_t iif = m_ipv4->GetInterfaceForDevice (idev);
        Ipv4Address dst = header.GetDestination ();
        Ipv4Address origin = header.GetSource ();

        DeferredRouteOutputTag tag; //FIXME since I have to check if it's in origin for it to work it means I'm not taking some tag out...
        //如果有推迟的tag标志同时自己是发送源就就推迟

        if (p->PeekPacketTag (tag) && IsMyOwnAddress (origin))
        {
                Ptr<Packet> packet = p->Copy (); //FIXME ja estou a abusar de tirar tags
                packet->RemovePacketTag(tag);
                DeferredRouteOutput (packet, header, ucb, ecb);
                return true;
        }

        // Ptr<Packet> packet = p->Copy ();
        // TypeHeader tHeader (GPSRTYPE_POS);
        // //去掉packet的包头
        // packet->RemoveHeader (tHeader);
        // //这个部分没有写，当作都valid
        // if (!tHeader.IsValid ())
        // {
        //         NS_LOG_DEBUG ("error received " << packet->GetUid () << " with unknown type received: " << tHeader.Get () << ". Ignored");
        //         NS_LOG_DEBUG ("Routing input ReceivePacket error destination"<<dst<<"source"<<origin);
        //         return false;
        // }

        //如果是目的节点的包，那么就拆包
        if (m_ipv4->IsDestinationAddress (dst, iif))
        {

                Ptr<Packet> packet = p->Copy ();
                TypeHeader tHeader (GPSRTYPE_POS);
                //去掉packet的包头
                packet->RemoveHeader (tHeader);
                //这个部分没有写，当作都valid
                if (!tHeader.IsValid ())
                {
                        NS_LOG_DEBUG ("GPSR message " << packet->GetUid () << " with unknown type received: " << tHeader.Get () << ". Ignored");
                        NS_LOG_DEBUG ("RoutingInput meet error");
                        return false;
                }
                NS_LOG_DEBUG ("Received packet");
                //如果是POS的包，就把POS的包头再去掉，所以不需要了解pos的信息了，直接去掉）
                if (tHeader.Get () == GPSRTYPE_POS)
                {
                        PositionHeader phdr;
                        packet->RemoveHeader (phdr);
                }

                //判断是广播接受还是单播接受到的包
                if (dst != m_ipv4->GetAddress (1, 0).GetBroadcast ())
                {
                        //通过unicast传到的
                        NS_LOG_LOGIC ("Unicast local delivery to " << dst);
                }
                else
                {
                        //通过broadcast传到dst的
                        NS_LOG_LOGIC ("Broadcast local delivery to " << dst);
                }

                //LocalDeliverCallback对象初始化，iff是mac地址.
                //是目的节点，给自己
                lcb (packet, header, iif);
                return true;
        }
        // //如果收到是hello，就不往前传了？
        // if (dst == m_ipv4->GetAddress (1, 0).GetBroadcast ())
        // {
        //   NS_LOG_LOGIC ("Receive broadcast hello " << dst);
        //   return true;
        // }
        Ptr<Packet> packet = p->Copy ();
        //如果不是目的节点继续向前传递
        // if(packet->GetSize()!=86) //防止回去的不丢
        // {
        if(packet->GetSize()!=90)
        {
                UdpHeader udpHeader;

                packet->RemoveHeader(udpHeader);
                NS_LOG_DEBUG("packet input"<<packet->GetSize());
        }
        else
        {
                packet->RemoveAtStart(4);
                NS_LOG_DEBUG("packet input"<<packet->GetSize());
        }
      //}
        return Forwarding (packet, header, ucb, ecb);
        //return Forwarding (p, header, ucb, ecb);
}


Ptr<Ipv4Route>
RoutingProtocol::RouteOutput (Ptr<Packet> p, const Ipv4Header &header,
                              Ptr<NetDevice> oif, Socket::SocketErrno &sockerr)
{
        NS_LOG_FUNCTION (this << header << (oif ? oif->GetIfIndex () : 0));
        NS_LOG_DEBUG("packet"<<p->GetSize());
        if (!p)
        {
                return LoopbackRoute (header, oif); // later
        }
        if (m_socketAddresses.empty ())
        {
                sockerr = Socket::ERROR_NOROUTETOHOST;
                NS_LOG_LOGIC ("No gpsr interfaces");
                Ptr<Ipv4Route> route;
                return route;
        }



        sockerr = Socket::ERROR_NOTERROR;
        Ptr<Ipv4Route> route = Create<Ipv4Route> ();
        Ipv4Address dst = header.GetDestination ();


        Vector dstPos = Vector (1, 0, 0);

        if (!(dst == m_ipv4->GetAddress (1, 0).GetBroadcast ()))
        {
                dstPos = m_locationService->GetPosition (dst);
        }
        //if received hello boardcast  TODO 还需要验证,应该如何处理
        else
        {
                NS_LOG_DEBUG("send broadcast hello from"<<header.GetSource()<<"TODO fix it ");

                route->SetDestination (dst);
                if (header.GetSource () == Ipv4Address ("102.102.102.102"))
                {
                        route->SetSource (m_ipv4->GetAddress (1, 0).GetLocal ());
                }
                else
                {
                        route->SetSource (header.GetSource ());
                }
                route->SetOutputDevice (m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (route->GetSource ())));
                route->SetDestination (header.GetDestination ());
                NS_ASSERT (route != 0);
                NS_LOG_DEBUG ("Exist route to " << route->GetDestination () << " from interface " << route->GetSource ());
                if (oif != 0 && route->GetOutputDevice () != oif)
                {
                        NS_LOG_DEBUG ("Output device doesn't match. Dropped.");
                        sockerr = Socket::ERROR_NOROUTETOHOST;
                        return Ptr<Ipv4Route> ();
                }
                return route;
                // DeferredRouteOutputTag tag;
                // if (!p->PeekPacketTag (tag))
                // {
                //         p->AddPacketTag (tag);
                // }
                // return LoopbackRoute (header, oif);
        }


        if (CalculateDistance (dstPos, m_locationService->GetInvalidPosition ()) == 0 && m_locationService->IsInSearch (dst))
        {
                NS_LOG_DEBUG("Cant get desitant position to delay");
                DeferredRouteOutputTag tag;
                if (!p->PeekPacketTag (tag))
                {
                        p->AddPacketTag (tag);
                }
                return LoopbackRoute (header, oif);
        }

        Vector myPos;
        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();
        myPos.x = MM->GetPosition ().x;
        myPos.y = MM->GetPosition ().y;
        Vector myVec;
        myVec=MM->GetVelocity();


        Ipv4Address nextHop;

        if(m_neighbors.isNeighbour (dst))
        {
                nextHop = dst;
        }
        else
        {
                nextHop = m_neighbors.BestNeighbor (dstPos, myPos,myVec);
        }

        if (nextHop != Ipv4Address::GetZero ())
        {
                //packet add header
                NS_LOG_DEBUG("Add header in Output");
                uint32_t updated = (uint32_t) m_locationService->GetEntryUpdateTime (dst).GetSeconds ();

                TypeHeader tHeader (GPSRTYPE_POS);
                PositionHeader posHeader (dstPos.x, dstPos.y,  updated, (uint64_t) 0, (uint64_t) 0, (uint8_t) 0, myPos.x, myPos.y);
                p->AddHeader (posHeader);
                p->AddHeader (tHeader);

                NS_LOG_DEBUG ("Destination: " << dst<<"Position"<<dstPos); //需要考虑boardcast的地址，位置再1.0.0,source 设置是102.102.102.102

                route->SetDestination (dst);
                if (header.GetSource () == Ipv4Address ("102.102.102.102"))
                {
                        route->SetSource (m_ipv4->GetAddress (1, 0).GetLocal ());
                }
                else
                {
                        route->SetSource (header.GetSource ());
                }
                route->SetGateway (nextHop);
                route->SetOutputDevice (m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (route->GetSource ())));
                route->SetDestination (header.GetDestination ());
                NS_ASSERT (route != 0);
                NS_LOG_DEBUG ("Exist route to " << route->GetDestination () << " from interface " << route->GetSource ());
                if (oif != 0 && route->GetOutputDevice () != oif)
                {
                        NS_LOG_DEBUG ("Output device doesn't match. Dropped.");
                        sockerr = Socket::ERROR_NOROUTETOHOST;
                        return Ptr<Ipv4Route> ();
                }
                return route;
        }
        else
        {
                // //packet add header
                // uint32_t updated = (uint32_t) m_locationService->GetEntryUpdateTime (dst).GetSeconds ();
                //
                // TypeHeader tHeader (GPSRTYPE_POS);
                // PositionHeader posHeader (dstPos.x, dstPos.y,  updated, (uint64_t) 0, (uint64_t) 0, (uint8_t) 0, myPos.x, myPos.y);
                // p->AddHeader (posHeader);
                // p->AddHeader (tHeader);

                DeferredRouteOutputTag tag;
                if (!p->PeekPacketTag (tag))
                {
                        p->AddPacketTag (tag);
                }
                return LoopbackRoute (header, oif); //in RouteInput the recovery-mode is called
        }
}



//推迟路由输出
void
RoutingProtocol::DeferredRouteOutput (Ptr<const Packet> p, const Ipv4Header & header,
                                      UnicastForwardCallback ucb, ErrorCallback ecb)
{
        NS_LOG_FUNCTION (this << p << header);
        NS_ASSERT (p != 0 && p != Ptr<Packet> ());

        if (m_queue.GetSize () == 0)
        {
                CheckQueueTimer.Cancel ();
                //重新开始记时调度
                CheckQueueTimer.Schedule (Time ("500ms"));
        }
        //将发送的包入队
        QueueEntry newEntry (p, header, ucb, ecb);
        bool result = m_queue.Enqueue (newEntry);


        m_queuedAddresses.insert (m_queuedAddresses.begin (), header.GetDestination ());
        m_queuedAddresses.unique ();

        if (result)
        {
                NS_LOG_DEBUG ("Add packet " << p->GetUid () << " to queue. Protocol " << (uint16_t) header.GetProtocol ());
        }

}

//检查排队情况
void
RoutingProtocol::CheckQueue ()
{

        CheckQueueTimer.Cancel ();

        std::list<Ipv4Address> toRemove;

        for (std::list<Ipv4Address>::iterator i = m_queuedAddresses.begin (); i != m_queuedAddresses.end (); ++i)
        {
                //如果该地址发送了,那么就把地址放到需要删除的列表中，之后一起删除
                if (SendPacketFromQueue (*i)) //判断的过程同时调用发送packet函数
                {
                        //Insert in a list to remove later
                        toRemove.insert (toRemove.begin (), *i);
                }
        }

        //remove all that are on the list
        for (std::list<Ipv4Address>::iterator i = toRemove.begin (); i != toRemove.end (); ++i)
        {
                m_queuedAddresses.remove (*i);
        }

        if (!m_queuedAddresses.empty ()) //Only need to schedule if the queue is not empty
        {
                CheckQueueTimer.Schedule (Time ("500ms"));
        }
}


//从queue中发送，前面是直接发送？
bool
RoutingProtocol::SendPacketFromQueue (Ipv4Address dst)
{
        NS_LOG_FUNCTION (this);
        bool recovery = false;
        QueueEntry queueEntry;
        NS_LOG_DEBUG ("SendPacketFromQueue ");
        //先通过locationService找到目的节点的位置
        if (m_locationService->IsInSearch (dst))
        {
                return false;
        }

        if (!m_locationService->HasPosition (dst)) // Location-service stoped looking for the dst
        {
                //queue删除发送目的节点的位置的request，因为没有找到目的节点的位置
                m_queue.DropPacketWithDst (dst);
                NS_LOG_LOGIC ("Location Service did not find dst. Drop packet to " << dst);
                return true;
        }

        Vector myPos;
        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();
        myPos= MM->GetPosition ();
        Vector myVec;
        myVec=MM->GetVelocity();
        Ipv4Address nextHop;

        //如果目的节点就是邻居节点，那么直接传给目的节点
        if(m_neighbors.isNeighbour (dst))
        {
                nextHop = dst;
        }
        //否则，就寻找距离目的最近的邻居节点
        else{
                //找到目标节点坐标的位置
                Vector dstPos = m_locationService->GetPosition (dst);

                nextHop = m_neighbors.BestNeighbor (dstPos, myPos,myVec);
                if (nextHop == Ipv4Address::GetZero ())
                {
                        NS_LOG_LOGIC ("Fallback to recovery-mode. Packets to " << dst);
                        recovery = true;
                }

        }
        //开启recovery mode
        if(recovery)
        {

                Vector Position;
                Vector previousHop;
                uint32_t updated;

                //将目的节点的发送request出队，修改发送包的信息，更新包里面的本节点的地理位置信息（之前如果不是recovery）
                //因为调用recovery mode需要记录当前开始recovery mode的节点位置（recvPos），因而需要展开包给与当前节点的位置信息

                while(m_queue.Dequeue (dst, queueEntry))
                {
                        Ptr<Packet> p = ConstCast<Packet> (queueEntry.GetPacket ());
                        UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback ();
                        Ipv4Header header = queueEntry.GetIpv4Header ();

                        TypeHeader tHeader (GPSRTYPE_POS);
                        p->RemoveHeader (tHeader);
                        if (!tHeader.IsValid ())
                        {
                                NS_LOG_DEBUG ("GPSR message " << p->GetUid () << " with unknown type received: " << tHeader.Get () << ". Drop");
                                NS_LOG_DEBUG ("recovery-mode meet error");
                                return false;         // drop
                        }
                        if (tHeader.Get () == GPSRTYPE_POS)
                        {
                                PositionHeader hdr;
                                p->RemoveHeader (hdr);
                                Position.x = hdr.GetDstPosx ();
                                Position.y = hdr.GetDstPosy ();
                                updated = hdr.GetUpdated ();
                        }

                        PositionHeader posHeader (Position.x, Position.y,  updated, myPos.x, myPos.y, (uint8_t) 1, Position.x, Position.y);
                        p->AddHeader (posHeader);         //enters in recovery with last edge from Dst
                        p->AddHeader (tHeader);

                        RecoveryMode(dst, p, ucb, header);
                }
                return true;
        }


        Ptr<Ipv4Route> route = Create<Ipv4Route> ();
        route->SetDestination (dst);
        route->SetGateway (nextHop);

        // FIXME: Does not work for multiple interfaces
        route->SetOutputDevice (m_ipv4->GetNetDevice (1));

        while (m_queue.Dequeue (dst, queueEntry))
        {
                DeferredRouteOutputTag tag;
                Ptr<Packet> p = ConstCast<Packet> (queueEntry.GetPacket ());

                UnicastForwardCallback ucb = queueEntry.GetUnicastForwardCallback ();
                Ipv4Header header = queueEntry.GetIpv4Header ();

                if (header.GetSource () == Ipv4Address ("102.102.102.102"))
                {
                        route->SetSource (m_ipv4->GetAddress (1, 0).GetLocal ());
                        header.SetSource (m_ipv4->GetAddress (1, 0).GetLocal ());
                }
                else
                {
                        route->SetSource (header.GetSource ());
                }
                ucb (route, p, header);
        }
        return true;
}


void
RoutingProtocol::RecoveryMode(Ipv4Address dst, Ptr<Packet> p, UnicastForwardCallback ucb, Ipv4Header header){

        Vector Position;
        Vector previousHop;
        uint32_t updated;
        uint64_t positionX;
        uint64_t positionY;
        Vector myPos;
        Vector recPos;

        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();
        positionX = MM->GetPosition ().x;
        positionY = MM->GetPosition ().y;
        myPos.x = positionX;
        myPos.y = positionY;


        //因为recovery需要记录中途节点的位置（lastPos），需要展开包，记录下当前节点的位置信息
        TypeHeader tHeader (GPSRTYPE_POS);
        p->RemoveHeader (tHeader);
        if (!tHeader.IsValid ())
        {
                NS_LOG_DEBUG ("GPSR message " << p->GetUid () << " with unknown type received: " << tHeader.Get () << ". Drop");
                return; // drop
        }
        if (tHeader.Get () == GPSRTYPE_POS)
        {
                PositionHeader hdr;
                p->RemoveHeader (hdr);
                Position.x = hdr.GetDstPosx ();
                Position.y = hdr.GetDstPosy ();
                updated = hdr.GetUpdated ();
                recPos.x = hdr.GetRecPosx ();
                recPos.y = hdr.GetRecPosy ();
                previousHop.x = hdr.GetLastPosx ();
                previousHop.y = hdr.GetLastPosy ();
        }

        PositionHeader posHeader (Position.x, Position.y,  updated, recPos.x, recPos.y, (uint8_t) 1, myPos.x, myPos.y);
        p->AddHeader (posHeader);
        p->AddHeader (tHeader);



        Ipv4Address nextHop = m_neighbors.BestAngle (previousHop, myPos); // best angle 来寻找
        if (nextHop == Ipv4Address::GetZero ())
        {
                return;
        }

        Ptr<Ipv4Route> route = Create<Ipv4Route> ();
        route->SetDestination (dst);
        route->SetGateway (nextHop);

        // FIXME: Does not work for multiple interfaces
        route->SetOutputDevice (m_ipv4->GetNetDevice (1));
        route->SetSource (header.GetSource ());

        ucb (route, p, header);
        return;
}

//bind socket to interface
void
RoutingProtocol::NotifyInterfaceUp (uint32_t interface)
{
        NS_LOG_FUNCTION (this << m_ipv4->GetAddress (interface, 0).GetLocal ());
        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
        //gpsr只能每个接口一个ip地址
        if (l3->GetNAddresses (interface) > 1)
        {
                NS_LOG_WARN ("GPSR does not work with more then one address per each interface.");
        }

        Ipv4InterfaceAddress iface = l3->GetAddress (interface, 0);
        //如果已经开启就返回
        if (iface.GetLocal () == Ipv4Address ("127.0.0.1"))
        {
                return;
        }

        // Create a socket to listen only on this interface
        Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
                                                   UdpSocketFactory::GetTypeId ());
        NS_ASSERT (socket != 0);
        socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvGPSR, this));
        socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), GPSR_PORT));
        socket->BindToNetDevice (l3->GetNetDevice (interface));
        socket->SetAllowBroadcast (true);
        socket->SetAttribute ("IpTtl", UintegerValue (1));
        m_socketAddresses.insert (std::make_pair (socket, iface));


        // Allow neighbor manager use this interface for layer 2 feedback if possible
        Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));
        Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice> ();
        if (wifi == 0)
        {
                return;
        }
        Ptr<WifiMac> mac = wifi->GetMac ();
        if (mac == 0)
        {
                return;
        }

        mac->TraceConnectWithoutContext ("TxErrHeader", m_neighbors.GetTxErrorCallback ());

}

//接受到socket的包
void
RoutingProtocol::RecvGPSR (Ptr<Socket> socket)
{
        NS_LOG_FUNCTION (this << socket);
        Address sourceAddress;
        Ptr<Packet> packet = socket->RecvFrom (sourceAddress);
        //RemoveHeader
        // TypeHeader tHeader1 (GPSRTYPE_HELLO);
        // packet->RemoveHeader (tHeader1);
        // PositionHeader pHeader ;
        // packet->RemoveHeader (pHeader);
        //packet->RemoveAtStart(54);
        NS_LOG_DEBUG("received hello packet"<<packet->GetSize());

        TypeHeader tHeader (GPSRTYPE_HELLO);
        packet->RemoveHeader (tHeader);
        if (!tHeader.IsValid ())
        {
                NS_LOG_DEBUG ("GPSR message " << packet->GetUid () << " with unknown type received: " << tHeader.Get () << ". Ignored");
                NS_LOG_DEBUG ("RecvGPSR meet error");
                return;
        }

        HelloHeader hdr;
        packet->RemoveHeader (hdr);
        Vector Position;
        Position.x = hdr.GetOriginPosx ();
        Position.y = hdr.GetOriginPosy ();
        InetSocketAddress inetSourceAddr = InetSocketAddress::ConvertFrom (sourceAddress);
        Ipv4Address sender = inetSourceAddr.GetIpv4 ();
        Ipv4Address receiver = m_socketAddresses[socket].GetLocal ();
        NS_LOG_DEBUG("update position"<<Position.x<<Position.y );
        //更新neighbor的信息
        UpdateRouteToNeighbor (sender, receiver, Position);

}


void
RoutingProtocol::UpdateRouteToNeighbor (Ipv4Address sender, Ipv4Address receiver, Vector Pos)
{
        m_neighbors.AddEntry (sender, Pos);
}


//关闭interface的socket
void
RoutingProtocol::NotifyInterfaceDown (uint32_t interface)
{
        NS_LOG_FUNCTION (this << m_ipv4->GetAddress (interface, 0).GetLocal ());

        // Disable layer 2 link state monitoring (if possible)
        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
        Ptr<NetDevice> dev = l3->GetNetDevice (interface);
        Ptr<WifiNetDevice> wifi = dev->GetObject<WifiNetDevice> ();
        if (wifi != 0)
        {
                Ptr<WifiMac> mac = wifi->GetMac ()->GetObject<AdhocWifiMac> ();
                if (mac != 0)
                {
                        mac->TraceDisconnectWithoutContext ("TxErrHeader",
                                                            m_neighbors.GetTxErrorCallback ());
                }
        }

        // Close socket
        Ptr<Socket> socket = FindSocketWithInterfaceAddress (m_ipv4->GetAddress (interface, 0));
        NS_ASSERT (socket);
        socket->Close ();
        m_socketAddresses.erase (socket);
        if (m_socketAddresses.empty ())
        {
                NS_LOG_LOGIC ("No gpsr interfaces");
                m_neighbors.Clear ();
                m_locationService->Clear ();
                return;
        }
}


//根据interface地址找到socket
Ptr<Socket>
RoutingProtocol::FindSocketWithInterfaceAddress (Ipv4InterfaceAddress addr ) const
{
        NS_LOG_FUNCTION (this << addr);
        for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator j =
                     m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
        {
                Ptr<Socket> socket = j->first;
                Ipv4InterfaceAddress iface = j->second;
                if (iface == addr)
                {
                        return socket;
                }
        }
        Ptr<Socket> socket;
        return socket;
}


//增加地址
void RoutingProtocol::NotifyAddAddress (uint32_t interface, Ipv4InterfaceAddress address)
{
        NS_LOG_FUNCTION (this << " interface " << interface << " address " << address);
        Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
        if (!l3->IsUp (interface))
        {
                return;
        }
        if (l3->GetNAddresses ((interface) == 1))
        {
                Ipv4InterfaceAddress iface = l3->GetAddress (interface, 0);
                Ptr<Socket> socket = FindSocketWithInterfaceAddress (iface);
                if (!socket)
                {
                        if (iface.GetLocal () == Ipv4Address ("127.0.0.1"))
                        {
                                return;
                        }
                        // Create a socket to listen only on this interface
                        Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
                                                                   UdpSocketFactory::GetTypeId ());
                        NS_ASSERT (socket != 0);
                        socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvGPSR,this));
                        // Bind to any IP address so that broadcasts can be received
                        socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), GPSR_PORT));
                        socket->BindToNetDevice (l3->GetNetDevice (interface));
                        socket->SetAllowBroadcast (true);
                        m_socketAddresses.insert (std::make_pair (socket, iface));

                        Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));
                }
        }
        else
        {
                NS_LOG_LOGIC ("GPSR does not work with more then one address per each interface. Ignore added address");
        }
}
//删除出地址
void
RoutingProtocol::NotifyRemoveAddress (uint32_t i, Ipv4InterfaceAddress address)
{
        NS_LOG_FUNCTION (this);
        Ptr<Socket> socket = FindSocketWithInterfaceAddress (address);
        if (socket)
        {

                m_socketAddresses.erase (socket);
                Ptr<Ipv4L3Protocol> l3 = m_ipv4->GetObject<Ipv4L3Protocol> ();
                if (l3->GetNAddresses (i))
                {
                        Ipv4InterfaceAddress iface = l3->GetAddress (i, 0);
                        // Create a socket to listen only on this interface
                        Ptr<Socket> socket = Socket::CreateSocket (GetObject<Node> (),
                                                                   UdpSocketFactory::GetTypeId ());
                        NS_ASSERT (socket != 0);
                        socket->SetRecvCallback (MakeCallback (&RoutingProtocol::RecvGPSR, this));
                        // Bind to any IP address so that broadcasts can be received
                        socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), GPSR_PORT));
                        socket->SetAllowBroadcast (true);
                        m_socketAddresses.insert (std::make_pair (socket, iface));

                        // Add local broadcast record to the routing table
                        Ptr<NetDevice> dev = m_ipv4->GetNetDevice (m_ipv4->GetInterfaceForAddress (iface.GetLocal ()));

                }
                if (m_socketAddresses.empty ())
                {
                        NS_LOG_LOGIC ("No gpsr interfaces");
                        m_neighbors.Clear ();
                        m_locationService->Clear ();
                        return;
                }
        }
        else
        {
                NS_LOG_LOGIC ("Remove address not participating in GPSR operation");
        }
}

void
RoutingProtocol::SetIpv4 (Ptr<Ipv4> ipv4)
{
        NS_ASSERT (ipv4 != 0);
        NS_ASSERT (m_ipv4 == 0);

        m_ipv4 = ipv4;

        //当HelloIntercalTimer到时间了 就在FIRST_JITTER时间后调用HelloTimerExpire
        HelloIntervalTimer.SetFunction (&RoutingProtocol::HelloTimerExpire, this);
        HelloIntervalTimer.Schedule (FIRST_JITTER);

        //Schedule only when it has packets on queue
        CheckQueueTimer.SetFunction (&RoutingProtocol::CheckQueue, this);

        Simulator::ScheduleNow (&RoutingProtocol::Start, this);
}

void
RoutingProtocol::HelloTimerExpire ()
{
        SendHello ();
        HelloIntervalTimer.Cancel ();
        //新建一个时间延时为HelloInterval + JITTER的Timer
        HelloIntervalTimer.Schedule (HelloInterval + JITTER);
}

//Hello包的发送
void
RoutingProtocol::SendHello ()
{
        NS_LOG_FUNCTION (this);
        double positionX;
        double positionY;

        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();

        positionX = MM->GetPosition ().x;
        positionY = MM->GetPosition ().y;

        for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator j = m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
        {
                Ptr<Socket> socket = j->first;
                Ipv4InterfaceAddress iface = j->second;
                HelloHeader helloHeader (((uint64_t) positionX),((uint64_t) positionY));

                Ptr<Packet> packet = Create<Packet> ();
                packet->AddHeader (helloHeader);
                TypeHeader tHeader (GPSRTYPE_HELLO);
                packet->AddHeader (tHeader);
                // Send to all-hosts broadcast if on /32 addr, subnet-directed otherwise
                Ipv4Address destination;
                if (iface.GetMask () == Ipv4Mask::GetOnes ())
                {
                        destination = Ipv4Address ("255.255.255.255");
                        NS_LOG_DEBUG("Send hello to destination"<<destination );
                }
                else
                {
                        destination = iface.GetBroadcast ();
                        NS_LOG_DEBUG("Send hello to destination"<<destination );
                }
                socket->SendTo (packet, 0, InetSocketAddress (destination, GPSR_PORT));

        }
}

bool
RoutingProtocol::IsMyOwnAddress (Ipv4Address src)
{
        NS_LOG_FUNCTION (this << src);
        for (std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator j =
                     m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
        {
                Ipv4InterfaceAddress iface = j->second;
                if (src == iface.GetLocal ())
                {
                        return true;
                }
        }
        return false;
}




void
RoutingProtocol::Start ()
{
        NS_LOG_FUNCTION (this);
        m_queuedAddresses.clear ();

        //FIXME ajustar timer, meter valor parametrizavel
        Time tableTime ("2s");

        switch (LocationServiceName)
        {
        case GPSR_LS_GOD:
                NS_LOG_DEBUG ("GodLS in use");
                m_locationService = CreateObject<GodLocationService> ();
                break;
        case GPSR_LS_RLS:
                NS_LOG_UNCOND ("RLS not yet implemented");
                break;
        }

}


//返回开始的节点
Ptr<Ipv4Route>
RoutingProtocol::LoopbackRoute (const Ipv4Header & hdr, Ptr<NetDevice> oif)
{
        NS_LOG_FUNCTION (this << hdr);
        m_lo = m_ipv4->GetNetDevice (0);
        NS_ASSERT (m_lo != 0);
        Ptr<Ipv4Route> rt = Create<Ipv4Route> ();
        rt->SetDestination (hdr.GetDestination ());

        std::map<Ptr<Socket>, Ipv4InterfaceAddress>::const_iterator j = m_socketAddresses.begin ();
        if (oif)
        {
                // Iterate to find an address on the oif device
                for (j = m_socketAddresses.begin (); j != m_socketAddresses.end (); ++j)
                {
                        Ipv4Address addr = j->second.GetLocal ();
                        int32_t interface = m_ipv4->GetInterfaceForAddress (addr);
                        if (oif == m_ipv4->GetNetDevice (static_cast<uint32_t> (interface)))
                        {
                                rt->SetSource (addr);
                                break;
                        }
                }
        }
        else
        {
                rt->SetSource (j->second.GetLocal ());
        }
        NS_ASSERT_MSG (rt->GetSource () != Ipv4Address (), "Valid GPSR source address not found");
        rt->SetGateway (Ipv4Address ("127.0.0.1"));
        NS_LOG_DEBUG("return to LoopbackRoute");
        rt->SetOutputDevice (m_lo);
        return rt;
}


int
RoutingProtocol::GetProtocolNumber (void) const
{
        return GPSR_PORT;
}

//源节点而不是中间节点，增加的包头(不是内部packet包头）而是route包头AddHeaders),好像没有用到
void
RoutingProtocol::AddHeaders (Ptr<Packet> p, Ipv4Address source, Ipv4Address destination, uint8_t protocol, Ptr<Ipv4Route> route)
{

        NS_LOG_FUNCTION (this << " source " << source << " destination " << destination);
        NS_LOG_DEBUG ("Call Add Headers function");
        Vector myPos;
        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();
        myPos.x = MM->GetPosition ().x;
        myPos.y = MM->GetPosition ().y;
        Vector myVec;
        myVec=MM->GetVelocity();
        //这里没有recoverymode？因为后面forward的函数使用了，这里的nexthop只是一个初始化而已
        Ipv4Address nextHop;

        if(m_neighbors.isNeighbour (destination))
        {
                nextHop = destination;
        }
        else
        {
                nextHop = m_neighbors.BestNeighbor (m_locationService->GetPosition (destination), myPos,myVec);
        }

        uint16_t positionX = 0;
        uint16_t positionY = 0;
        uint32_t hdrTime = 0;

        if(destination != m_ipv4->GetAddress (1, 0).GetBroadcast ())
        {
                positionX = m_locationService->GetPosition (destination).x;
                positionY = m_locationService->GetPosition (destination).y;
                hdrTime = (uint32_t) m_locationService->GetEntryUpdateTime (destination).GetSeconds ();
        }

        PositionHeader posHeader (positionX, positionY,  hdrTime, (uint64_t) 0,(uint64_t) 0, (uint8_t) 0, myPos.x, myPos.y);
        p->AddHeader (posHeader);
        TypeHeader tHeader (GPSRTYPE_POS);
        p->AddHeader (tHeader);

        m_downTarget (p, source, destination, protocol, route);

}

//fowading 是中间点传输
bool
RoutingProtocol::Forwarding (Ptr<const Packet> packet, const Ipv4Header & header,
                             UnicastForwardCallback ucb, ErrorCallback ecb)
{
        Ptr<Packet> p = packet->Copy ();
        NS_LOG_FUNCTION (this);
        Ipv4Address dst = header.GetDestination ();
        Ipv4Address origin = header.GetSource ();


        m_neighbors.Purge ();

        uint32_t updated = 0;
        Vector Position;
        Vector RecPosition;
        uint8_t inRec = 0;

        TypeHeader tHeader (GPSRTYPE_POS);
        PositionHeader hdr;

        //尝试看看是不是tag的问题，好像不是
        //DeferredRouteOutputTag tag;
        //p->RemovePacketTag(tag);

        p->RemoveHeader (tHeader);

        if (!tHeader.IsValid ())
        //if (!tHeader.IsValid ())
        {
                NS_LOG_DEBUG ("GPSR message " << p->GetUid () << " with unknown type received: " << tHeader.Get () << " Drop");
                NS_LOG_DEBUG ("Forwarding meet packet drop because tHeader Deserialize failed "<<tHeader.IsValid ());
                return false; // drop
        }
        if (tHeader.Get () == GPSRTYPE_POS)
        {

                p->RemoveHeader (hdr);
                Position.x = hdr.GetDstPosx ();
                Position.y = hdr.GetDstPosy ();
                updated = hdr.GetUpdated ();
                RecPosition.x = hdr.GetRecPosx ();
                RecPosition.y = hdr.GetRecPosy ();
                inRec = hdr.GetInRec ();
        }

        Vector myPos;
        Ptr<MobilityModel> MM = m_ipv4->GetObject<MobilityModel> ();
        myPos.x = MM->GetPosition ().x;
        myPos.y = MM->GetPosition ().y;
        Vector myVec;
        myVec=MM->GetVelocity();


        //如果找到的节点比之前开始的位置节点到终点的距离近，就跳出recovery mode
        if(inRec == 1 && CalculateDistance (myPos, Position) < CalculateDistance (RecPosition, Position)) {
                inRec = 0;
                hdr.SetInRec(0);
                NS_LOG_LOGIC ("No longer in Recovery to " << dst << " in " << myPos);
        }

        //如果再recovery mod，同时本节点仍不比到目的距离近（没满足上面的条件） 那么就继续向前发
        if(inRec) {
                p->AddHeader (hdr);
                p->AddHeader (tHeader); //put headers back so that the RecoveryMode is compatible with Forwarding and SendFromQueue
                RecoveryMode (dst, p, ucb, header);
                return true;
        }


        //更新目的节点的位置信息
        uint32_t myUpdated = (uint32_t) m_locationService->GetEntryUpdateTime (dst).GetSeconds ();
        if (myUpdated > updated) //check if node has an update to the position of destination
        {
                Position.x = m_locationService->GetPosition (dst).x;
                Position.y = m_locationService->GetPosition (dst).y;
                updated = myUpdated;
        }


        Ipv4Address nextHop;

        if(m_neighbors.isNeighbour (dst))
        {
                nextHop = dst;
        }
        else
        {
                nextHop = m_neighbors.BestNeighbor (Position, myPos, myVec);
        }

        if (nextHop != Ipv4Address::GetZero ())
        {
                //如果是position 就新建新的破碎Header 增加到里面

                PositionHeader posHeader (Position.x, Position.y,  updated, (uint64_t) 0, (uint64_t) 0, (uint8_t) 0, myPos.x, myPos.y);
                p->AddHeader (posHeader);
                p->AddHeader (tHeader);

                //add udp headers
                if(packet->GetSize()!=86)
                {
                        UdpHeader udpHeader;
                        p->AddHeader(udpHeader);
                }

                Ptr<NetDevice> oif = m_ipv4->GetObject<NetDevice> ();
                Ptr<Ipv4Route> route = Create<Ipv4Route> ();
                route->SetDestination (dst);
                route->SetSource (header.GetSource ());
                route->SetGateway (nextHop);

                // FIXME: Does not work for multiple interfaces
                route->SetOutputDevice (m_ipv4->GetNetDevice (1));
                route->SetDestination (header.GetDestination ());
                NS_ASSERT (route != 0);
                NS_LOG_DEBUG ("Exist route to " << route->GetDestination () << " from interface " << route->GetOutputDevice ());
                NS_LOG_DEBUG (route->GetOutputDevice () << " forwarding to " << dst << " from " << origin << " through " << route->GetGateway () << " packet " << p->GetUid ());

                ucb (route, p, header);
                //ucb (route, p, header);
                return true;

        }
//change here
        // else
        // {
        //         nextHop = m_neighbors.BestNeighbor (Position, myPos, myVec);
        //
        //
        // if (nextHop != Ipv4Address::GetZero ())
        // {
        //         //如果是position 就新建新的破碎Header 增加到里面
        //
        //         PositionHeader posHeader (Position.x, Position.y,  updated, (uint64_t) 0, (uint64_t) 0, (uint8_t) 0, myPos.x, myPos.y);
        //         p->AddHeader (posHeader);
        //         p->AddHeader (tHeader);
        //
        //
        //         Ptr<NetDevice> oif = m_ipv4->GetObject<NetDevice> ();
        //         Ptr<Ipv4Route> route = Create<Ipv4Route> ();
        //         route->SetDestination (dst);
        //         route->SetSource (header.GetSource ());
        //         route->SetGateway (nextHop);
        //
        //         // FIXME: Does not work for multiple interfaces
        //         route->SetOutputDevice (m_ipv4->GetNetDevice (1));
        //         route->SetDestination (header.GetDestination ());
        //         NS_ASSERT (route != 0);
        //         NS_LOG_DEBUG ("Exist route to " << route->GetDestination () << " from interface " << route->GetOutputDevice ());
        //         NS_LOG_DEBUG (route->GetOutputDevice () << " forwarding to " << dst << " from " << origin << " through " << route->GetGateway () << " packet " << p->GetUid ());
        //
        //         ucb (route, p, header);
        //         return true;
        //
        // }
        //}

        hdr.SetInRec(1);
        hdr.SetRecPosx (myPos.x);
        hdr.SetRecPosy (myPos.y);
        hdr.SetLastPosx (Position.x); //when entering Recovery, the first edge is the Dst
        hdr.SetLastPosy (Position.y);


        p->AddHeader (hdr);
        p->AddHeader (tHeader);
        RecoveryMode (dst, p, ucb, header);

        NS_LOG_LOGIC ("Entering recovery-mode to " << dst << " in " << m_ipv4->GetAddress (1, 0).GetLocal ());
        return true;
}



void
RoutingProtocol::SetDownTarget (IpL4Protocol::DownTargetCallback callback)
{
        m_downTarget = callback;
}


IpL4Protocol::DownTargetCallback
RoutingProtocol::GetDownTarget (void) const
{
        return m_downTarget;
}


}

}

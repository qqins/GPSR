#include "gpsr-ptable.h"
#include "ns3/simulator.h"
#include "ns3/log.h"
#include <algorithm>
#include <cmath>

NS_LOG_COMPONENT_DEFINE ("GpsrTable");


namespace ns3 {
namespace gpsr {

/*
   GPSR position table
 */

PositionTable::PositionTable ()
{
        m_txErrorCallback = MakeCallback (&PositionTable::ProcessTxError, this);
        m_entryLifeTime = Seconds (2); //FIXME fazer isto parametrizavel de acordo com tempo de hello

}

Time
PositionTable::GetEntryUpdateTime (Ipv4Address id)
{
        if (id == Ipv4Address::GetZero ())
        {
                return Time (Seconds (0));
        }
        std::map<Ipv4Address, Metrix>::iterator i = m_table.find (id);
        return i->second.time; //返回记录的当时时间
}

/**
 * \brief Adds entry in position table
 */
//TODO finish velocity
void
PositionTable::AddEntry (Ipv4Address id, Vector position)
{
        std::map<Ipv4Address, Metrix >::iterator i = m_table.find (id);

        //id在table中，更新table,增加位置和速度信息
        if (i != m_table.end () || id.IsEqual (i->first))
        {
                m_table.erase (id);
                Metrix metrix;
                metrix.position=position;
                //metrix.velocity=velocity;
                metrix.time=Simulator::Now ();
                m_table.insert (std::make_pair (id, metrix));
                return; // 必须返回，否则后面没有办法进行
        }

        //id不在table，增加id
        Metrix metrix;
        metrix.position=position;
        metrix.time=Simulator::Now ();
        m_table.insert (std::make_pair (id, metrix));

}

/**
 * \brief Deletes entry in position table
 */
void PositionTable::DeleteEntry (Ipv4Address id)
{
        m_table.erase (id);
}

/**
 * \brief Gets position from position table
 * \param id Ipv4Address to get position from
 * \return Position of that id or NULL if not known
 */

//获取对应address节点的位置信息
//TODO 增加获取获取速度信息——>更可以考虑获取传输速率信息

Vector
PositionTable::GetPosition (Ipv4Address id)
{

        NodeList::Iterator listEnd = NodeList::End ();
        for (NodeList::Iterator i = NodeList::Begin (); i != listEnd; i++)
        {
                Ptr<Node> node = *i;
                if (node->GetObject<Ipv4> ()->GetAddress (1, 0).GetLocal () == id)
                {
                        return node->GetObject<MobilityModel> ()->GetPosition ();
                }
        }
        return PositionTable::GetInvalidPosition ();

}

Vector
PositionTable::GetVelocity (Ipv4Address id)
{

        NodeList::Iterator listEnd = NodeList::End ();
        for (NodeList::Iterator i = NodeList::Begin (); i != listEnd; i++)
        {
                Ptr<Node> node = *i;
                if (node->GetObject<Ipv4> ()->GetAddress (1, 0).GetLocal () == id)
                {
                        return node->GetObject<MobilityModel> ()->GetVelocity ();
                }
        }
        return PositionTable::GetInvalidVelocity ();

}


/**
 * \brief Checks if a node is a neighbour
 * \param id Ipv4Address of the node to check
 * \return True if the node is neighbour, false otherwise
 */
bool
PositionTable::isNeighbour (Ipv4Address id)
{
        Purge();
        std::map<Ipv4Address, Metrix >::iterator i = m_table.find (id);
        //是邻居节点
        if (i != m_table.end () || id.IsEqual (i->first))
        {
                return true;
        }
        //不是邻居节点
        return false;
}


/**
 * \brief remove entries with expired lifetime
 */

//删除table中超过时间的信息
void
PositionTable::Purge ()
{

        if(m_table.empty ())
        {
                return;
        }

        std::list<Ipv4Address> toErase;

        std::map<Ipv4Address, Metrix>::iterator i = m_table.begin ();
        std::map<Ipv4Address, Metrix >::iterator listEnd = m_table.end ();

        for (; !(i == listEnd); i++)
        {

                if (m_entryLifeTime + GetEntryUpdateTime (i->first) <= Simulator::Now ())
                {
                        toErase.insert (toErase.begin (), i->first); //如果超过时间，增加到删除列表

                }
        }
        toErase.unique (); //唯一化

        std::list<Ipv4Address>::iterator end = toErase.end ();

        for (std::list<Ipv4Address>::iterator it = toErase.begin (); it != end; ++it)
        {

                m_table.erase (*it); //删除表中对应列表的地址id

        }
}

/**
 * \brief clears all entries
 */

//清空table列表中所有值
void
PositionTable::Clear ()
{
        m_table.clear ();
}

/**
 * \brief Gets next hop according to GPSR protocol
 * \param position the position of the destination node
 * \param nodePos the position of the node that has the packet
 * \return Ipv4Address of the next hop, Ipv4Address::GetZero () if no nighbour was found in greedy mode
 */

 //找最佳的传输节点 position是给定目的节点的位置，nodePos是源节点速度,nodeVec是发送节点速度
 //TODO 修改算法
//
// Ipv4Address
// PositionTable::BestNeighbor (Vector position, Vector nodePos, Vector nodeVec)
// {
//   Purge ();
//
//   double initialDistance = CalculateDistance (nodePos, position);
//
//   if (m_table.empty ())
//     {
//       NS_LOG_DEBUG ("My neighhood table is empty; My Position: " << nodePos);
//       return Ipv4Address::GetZero ();
//     }     //if table is empty (no neighbours)
//
//   std::list<Ipv4Address> candidate;
//   std::map<Ipv4Address, Metrix >::iterator i;
//
//   for (i = m_table.begin (); !(i == m_table.end ()); i++)
//     {
//
//       if (initialDistance>CalculateDistance (i->second.position, position))
//       //if (initialDistance>CalculateDistance ((i->second.position,position) position))
//         {
//           candidate.push_back(i->first);
//         }
//     }
//   if(candidate.empty())
//      return Ipv4Address::GetZero ();
//   else
//  {
//   std::list<Ipv4Address>::iterator i = candidate.begin ();
//   Ipv4Address bestFoundID = *candidate.begin();
//   double bestFoundPara = 0;
//   Vector bestPosition;
//   //在前进的邻接点找最优的
//   for (i = candidate.begin (); !(i == candidate.end ()); i++)
//     {
//       Vector tempv=GetVelocity(*i);
//       Vector tempp=GetPosition(*i);
//       double alpha=tempv.x-nodeVec.x;
//       double beta=tempp.x-nodePos.x;
//       double R=250;
//       double gama=tempv.y-nodeVec.y;
//       double sita=tempp.y-nodePos.y;
//
//       double tempt=(sqrt(pow(alpha,2)+pow(beta,2)*pow(R,2)-pow(alpha*sita-beta*gama,2))-(alpha*beta+gama*sita))/(pow(alpha,2)+pow(gama,2));
//
//       if (bestFoundPara < (pow(tempt,1)/pow(CalculateDistance (tempp, nodePos),0)))
//         {
//           bestFoundID = *i;
//           bestFoundPara = pow(tempt,1)/pow(CalculateDistance (tempp, nodePos),0);
//           bestPosition = tempp;
//         }
//
//     }
//     NS_LOG_DEBUG ("BestNeighbor ID: " <<bestFoundID<<"Begin ID" <<*candidate.begin () );
//     NS_LOG_DEBUG ("Send packet to Position: " << bestPosition<<" From position"<<nodePos);
//     return bestFoundID;
// }
// }

Ipv4Address
PositionTable::BestNeighbor (Vector position, Vector nodePos, Vector nodeVec)
{
  Purge ();

  double initialDistance = CalculateDistance (nodePos, position);

  if (m_table.empty ())
    {
      NS_LOG_DEBUG ("My neighhood table is empty; My Position: " << nodePos);
      return Ipv4Address::GetZero ();
    }     //if table is empty (no neighbours)

  std::list<Ipv4Address> candidate;
  std::map<Ipv4Address, Metrix >::iterator i;

  for (i = m_table.begin (); !(i == m_table.end ()); i++)
    {

      if (initialDistance>CalculateDistance (i->second.position, position))
      //if (initialDistance>CalculateDistance ((i->second.position,position) position))
        {
          candidate.push_back(i->first);
        }
    }
  if(candidate.empty())
     return Ipv4Address::GetZero ();
  else
 {
  std::list<Ipv4Address>::iterator i = candidate.begin ();
  Ipv4Address bestFoundID = *candidate.begin();
  double bestFoundPara = 0;
  Vector bestPosition;
  //在前进的邻接点找最优的
  for (i = candidate.begin (); !(i == candidate.end ()); i++)
    {
      Vector tempv=GetVelocity(*i);
      Vector tempp=GetPosition(*i);
      double alpha=tempv.x-nodeVec.x;
      double beta=tempp.x-nodePos.x;
      double R=250;
      double gama=tempv.y-nodeVec.y;
      double sita=tempp.y-nodePos.y;

      double tempt=(sqrt(pow(alpha,2)+pow(beta,2)*pow(R,2)-pow(alpha*sita-beta*gama,2))-(alpha*beta+gama*sita))/(pow(alpha,2)+pow(gama,2));

      if (bestFoundPara < (pow(tempt,1)*pow(CalculateDistance (tempp, nodePos),0)))
        {
          bestFoundID = *i;
          bestFoundPara = pow(tempt,1)*pow(CalculateDistance (tempp, nodePos),0);
          bestPosition = tempp;
        }

    }
    NS_LOG_DEBUG ("BestNeighbor ID: " <<bestFoundID<<"Begin ID" <<*candidate.begin () );
    NS_LOG_DEBUG ("Send packet to Position: " << bestPosition<<" From position"<<nodePos);
    return bestFoundID;
}
}

//  //找最佳的传输节点 position是给定目的节点的位置，nodePos是源节点速度,nodeVec是发送节点速度
//  //TODO 修改算法
//
// Ipv4Address
// PositionTable::BestNeighbor (Vector position, Vector nodePos, Vector nodeVec)
// {
//   Purge ();
//
//   double initialDistance = CalculateDistance (nodePos, position);
//
//   if (m_table.empty ())
//     {
//       NS_LOG_DEBUG ("BestNeighbor table is empty; Position: " << position);
//       return Ipv4Address::GetZero ();
//     }     //if table is empty (no neighbours)
//
//   Ipv4Address bestFoundID = m_table.begin ()->first;
//   double bestFoundDistance = CalculateDistance (m_table.begin ()->second.position, position); //计算邻居节点距离目的节点最短的节点
//   std::map<Ipv4Address, Metrix >::iterator i;
//
//   //找到邻居节点距离目的节点最近的那个邻居节点
//   for (i = m_table.begin (); !(i == m_table.end ()); i++)
//     {
//       if (bestFoundDistance > CalculateDistance (i->second.position, position))
//         {
//           bestFoundID = i->first;
//           bestFoundDistance = CalculateDistance (i->second.position, position);
//           NS_LOG_DEBUG ("Best distance "<<bestFoundDistance);
//         }
//     }
//   //找到最好的邻居节点，返回邻居节点address
//   if(initialDistance > bestFoundDistance)
//     return bestFoundID;
//
//   //如果邻居没有比源节点距离目的节点最近的节点，就返回没有找到，进行recovery-mode
//   else
//   {
//     NS_LOG_DEBUG ("No best ID ");
//     return Ipv4Address::GetZero (); //so it enters Recovery-mode
// }
// }

//
// //找最佳的传输节点 position是给定目的节点的位置，nodePos是源节点速度,nodeVec是发送节点速度
// //TODO 修改算法
//
// Ipv4Address
// PositionTable::BestNeighbor (Vector position, Vector nodePos, Vector nodeVec)
// {
//         Purge ();
//
//         double initialDistance = CalculateDistance (nodePos, position);
//
//         if (m_table.empty ())
//         {
//                 NS_LOG_DEBUG ("BestNeighbor table is empty; Position: " << position);
//                 return Ipv4Address::GetZero ();
//         } //if table is empty (no neighbours)
//
//         Ipv4Address bestFoundID = m_table.begin ()->first;
//         double bestFoundDistance = CalculateDistance (m_table.begin ()->second.position, position); //计算邻居节点距离目的节点最短的节点
//         std::map<Ipv4Address, Metrix >::iterator i;
//
//         //找到邻居节点距离目的节点最近的那个邻居节点
//         for (i = m_table.begin (); !(i == m_table.end ()); i++)
//         {
//           NodeList::Iterator listEnd = NodeList::End ();
//                 for (NodeList::Iterator j = NodeList::Begin (); j != listEnd; j++)
//                 {
//                         Ptr<Node> node = *j;
//                         if (node->GetObject<Ipv4> ()->GetAddress (1, 0).GetLocal () == i->first)
//                         {
//                                if (node->GetId()>39)
//                                bestFoundID=i->first;
//                                return bestFoundID;
//                         }
//                 }
//
//                 if (bestFoundDistance > CalculateDistance (i->second.position, position))
//                 {
//                         bestFoundID = i->first;
//                         bestFoundDistance = CalculateDistance (i->second.position, position);
//                 }
//         }
//         //找到最好的邻居节点，返回邻居节点address
//         if(initialDistance > bestFoundDistance)
//                 return bestFoundID;
//
//         //如果邻居没有比源节点距离目的节点最近的节点，就返回没有找到，进行recovery-mode
//         else
//                 return Ipv4Address::GetZero ();  //so it enters Recovery-mode
// }
//
//


/**
 * \brief Gets next hop according to GPSR recovery-mode protocol (right hand rule)
 * \param previousHop the position of the node that sent the packet to this node
 * \param nodePos the position of the destination node
 * \return Ipv4Address of the next hop, Ipv4Address::GetZero () if no nighbour was found in greedy mode
 */

//根据右手准则找邻居节点,没有邻居就返回address.getzero（）

Ipv4Address
PositionTable::BestAngle (Vector previousHop, Vector nodePos)
{
        Purge ();

        if (m_table.empty ())
        {
                NS_LOG_DEBUG (" Recovery-mode but neighbours table empty; Position: " << nodePos);
                return Ipv4Address::GetZero ();
        } //if table is empty (no neighbours)

        NS_LOG_DEBUG (" Recovery-mode start bestangle " << nodePos);
        double tmpAngle;
        Ipv4Address bestFoundID = Ipv4Address::GetZero ();
        double bestFoundAngle = 360;
        std::map<Ipv4Address, Metrix >::iterator i;

        for (i = m_table.begin (); !(i == m_table.end ()); i++)
        {
                tmpAngle = GetAngle(nodePos, previousHop, i->second.position);
                if (bestFoundAngle > tmpAngle && tmpAngle != 0)
                {
                        bestFoundID = i->first;
                        bestFoundAngle = tmpAngle;
                }
        }
        if(bestFoundID == Ipv4Address::GetZero ()) //only if the only neighbour is who sent the packet
        {
                bestFoundID = m_table.begin ()->first;
        }
        return bestFoundID;
}


//Gives angle between the vector CentrePos-Refpos to the vector CentrePos-node counterclockwise
double
PositionTable::GetAngle (Vector centrePos, Vector refPos, Vector node){
        double const PI = 4*atan(1);

        std::complex<double> A = std::complex<double>(centrePos.x,centrePos.y);
        std::complex<double> B = std::complex<double>(node.x,node.y);
        std::complex<double> C = std::complex<double>(refPos.x,refPos.y); //Change B with C if you want angles clockwise

        std::complex<double> AB; //reference edge
        std::complex<double> AC;
        std::complex<double> tmp;
        std::complex<double> tmpCplx;

        std::complex<double> Angle;

        AB = B - A;
        AB = (real(AB)/norm(AB)) + (std::complex<double>(0.0,1.0)*(imag(AB)/norm(AB)));

        AC = C - A;
        AC = (real(AC)/norm(AC)) + (std::complex<double>(0.0,1.0)*(imag(AC)/norm(AC)));

        tmp = log(AC/AB);
        tmpCplx = std::complex<double>(0.0,-1.0);
        Angle = tmp*tmpCplx;
        Angle *= (180/PI);
        if (real(Angle)<0)
                Angle = 360+real(Angle);

        return real(Angle);

}





/**
 * \ProcessTxError
 */
void PositionTable::ProcessTxError (WifiMacHeader const & hdr)
{
}



//FIXME ainda preciso disto agr que o LS ja n está aqui???????

/**
 * \brief Returns true if is in search for destionation
 */
bool PositionTable::IsInSearch (Ipv4Address id)
{
        return false;
}

bool PositionTable::HasPosition (Ipv4Address id)
{
        return true;
}


}   // gpsr
} // ns3

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/netanim-module.h"
#include "ns3/mobility-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("IntelligentTopologyAnalyzer");

#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"

// Optimized Configurations for VirtualBox
double g_throughputWeight = 10.0;
double g_delayWeight = 1.0;
double g_packetLossWeight = 50.0;
double g_stopTime = 5.0; // Reduced to 5 seconds for fast execution
uint32_t g_maxPackets = 5000;
uint32_t g_packetSize = 1024;
double g_interval = 0.001; // 1ms interval -> 1000 pkts/sec -> ~8Mbps

struct TopologyMetrics {
    std::string name;
    double throughput;
    double delay;
    double packetLoss;
    double score;
    uint64_t txPackets;
    uint64_t rxPackets;
};

void calculateScore(TopologyMetrics& metrics) {
    metrics.score = (g_throughputWeight * metrics.throughput) 
                  - (g_delayWeight * metrics.delay) 
                  - (g_packetLossWeight * metrics.packetLoss);
}

TopologyMetrics parseMetrics(Ptr<FlowMonitor> monitor, FlowMonitorHelper& flowmon, std::string name) {
    monitor->CheckForLostPackets ();
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

    uint64_t totalRxBytes = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalRxPackets = 0;
    double totalDelay = 0.0;

    for (auto const &stat : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (stat.first);
        if (t.destinationPort == 9) { 
            totalRxBytes += stat.second.rxBytes;
            totalTxPackets += stat.second.txPackets;
            totalRxPackets += stat.second.rxPackets;
            totalDelay += stat.second.delaySum.GetSeconds ();
        }
    }

    TopologyMetrics metrics;
    metrics.name = name;
    metrics.txPackets = totalTxPackets;
    metrics.rxPackets = totalRxPackets;

    double activeDuration = g_stopTime - 1.0; 
    metrics.throughput = (totalRxBytes * 8.0) / (activeDuration * 1000000.0);
    metrics.delay = (totalRxPackets > 0) ? (totalDelay / totalRxPackets) * 1000.0 : 0.0;
    metrics.packetLoss = (totalTxPackets > 0) ? ((totalTxPackets - totalRxPackets) / (double)totalTxPackets) * 100.0 : 0.0;
    
    calculateScore(metrics);
    return metrics;
}

// 1. Point-to-Point Topology
TopologyMetrics runPointToPoint() {
    std::cout << CYAN << "[1/5] Running Point-to-Point Topology..." << RESET << std::endl;
    NodeContainer nodes;
    nodes.Create(5);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps")); // Bottleneck for congestion
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    Ipv4AddressHelper address;
    Ipv4InterfaceContainer targetInterfaces;

    for (uint32_t i = 0; i < 4; ++i) {
        NodeContainer link(nodes.Get(i), nodes.Get(i+1));
        NetDeviceContainer devices = p2p.Install(link);
        
        std::ostringstream subnet;
        subnet << "10.1." << i + 1 << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        if (i == 3) targetInterfaces = ifaces; 
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(nodes.Get(4));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    UdpClientHelper client(destAddr, port);
    client.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client.SetAttribute("PacketSize", UintegerValue(g_packetSize));

    ApplicationContainer client1 = client.Install(nodes.Get(0));
    client1.Start(Seconds(1.0));
    client1.Stop(Seconds(g_stopTime));

    ApplicationContainer client2 = client.Install(nodes.Get(1));
    client2.Start(Seconds(1.1));
    client2.Stop(Seconds(g_stopTime));

    AnimationInterface anim("p2p_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); 
    
    // Optimized NetAnim positioning directly via anim.SetConstantPosition()
    for (uint32_t i = 0; i < 5; ++i) {
        anim.SetConstantPosition(nodes.Get(i), 10.0 + (i * 20.0), 50.0);
    }

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Point-to-Point");
    Simulator::Destroy();
    return metrics;
}

// 2. Bus Topology
TopologyMetrics runBusTopology() {
    std::cout << CYAN << "[2/5] Running Bus Topology..." << RESET << std::endl;
    NodeContainer nodes;
    nodes.Create(5);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("10Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    NetDeviceContainer devices = csma.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(nodes.Get(4));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = interfaces.GetAddress(4);

    UdpClientHelper client(destAddr, port);
    client.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client.SetAttribute("PacketSize", UintegerValue(g_packetSize));

    ApplicationContainer client1 = client.Install(nodes.Get(0));
    client1.Start(Seconds(1.0));
    client1.Stop(Seconds(g_stopTime));

    ApplicationContainer client2 = client.Install(nodes.Get(1));
    client2.Start(Seconds(1.1));
    client2.Stop(Seconds(g_stopTime));

    AnimationInterface anim("bus_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); 

    for (uint32_t i = 0; i < 5; ++i) {
        anim.SetConstantPosition(nodes.Get(i), 10.0 + (i * 20.0), 50.0);
    }

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Bus Topology");
    Simulator::Destroy();
    return metrics;
}

// 3. Star Topology
TopologyMetrics runStarTopology() {
    std::cout << CYAN << "[3/5] Running Star Topology..." << RESET << std::endl;
    NodeContainer hub;
    hub.Create(1);
    NodeContainer spokes;
    spokes.Create(4);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(hub);
    mobility.Install(spokes);

    InternetStackHelper stack;
    stack.Install(hub);
    stack.Install(spokes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    Ipv4AddressHelper address;
    Ipv4InterfaceContainer targetInterfaces;

    for (uint32_t i = 0; i < 4; ++i) {
        NodeContainer link(hub.Get(0), spokes.Get(i));
        NetDeviceContainer devices = p2p.Install(link);
        std::ostringstream subnet;
        subnet << "10.3." << i + 1 << ".0";
        address.SetBase(subnet.str().c_str(), "255.255.255.0");
        Ipv4InterfaceContainer ifaces = address.Assign(devices);
        
        if (i == 3) targetInterfaces = ifaces; 
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(spokes.Get(3));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    UdpClientHelper client(destAddr, port);
    client.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client.SetAttribute("PacketSize", UintegerValue(g_packetSize));

    ApplicationContainer client1 = client.Install(spokes.Get(0));
    client1.Start(Seconds(1.0));
    client1.Stop(Seconds(g_stopTime));

    ApplicationContainer client2 = client.Install(spokes.Get(1));
    client2.Start(Seconds(1.1));
    client2.Stop(Seconds(g_stopTime));

    AnimationInterface anim("star_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); 

    anim.SetConstantPosition(hub.Get(0), 50.0, 50.0); // Center Hub
    anim.SetConstantPosition(spokes.Get(0), 50.0, 20.0); // Top
    anim.SetConstantPosition(spokes.Get(1), 80.0, 50.0); // Right
    anim.SetConstantPosition(spokes.Get(2), 50.0, 80.0); // Bottom
    anim.SetConstantPosition(spokes.Get(3), 20.0, 50.0); // Left

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Star Topology");
    Simulator::Destroy();
    return metrics;
}

// 4. Mesh Topology
TopologyMetrics runMeshTopology() {
    std::cout << CYAN << "[4/5] Running Mesh Topology..." << RESET << std::endl;
    NodeContainer nodes;
    nodes.Create(5);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    Ipv4AddressHelper address;
    int subnetIdx = 1;
    Ipv4Address addr_4_from_0;
    Ipv4Address addr_4_from_1;

    for (uint32_t i = 0; i < nodes.GetN(); ++i) {
        for (uint32_t j = i + 1; j < nodes.GetN(); ++j) {
            NodeContainer link(nodes.Get(i), nodes.Get(j));
            NetDeviceContainer devices = p2p.Install(link);

            std::ostringstream subnet;
            subnet << "10.4." << subnetIdx++ << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            Ipv4InterfaceContainer ifaces = address.Assign(devices);
            
            if (i == 0 && j == 4) addr_4_from_0 = ifaces.GetAddress(1);
            if (i == 1 && j == 4) addr_4_from_1 = ifaces.GetAddress(1);
        }
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(nodes.Get(4));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(g_stopTime));

    UdpClientHelper client1(addr_4_from_0, port);
    client1.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client1.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client1.SetAttribute("PacketSize", UintegerValue(g_packetSize));
    ApplicationContainer app1 = client1.Install(nodes.Get(0));
    app1.Start(Seconds(1.0));
    app1.Stop(Seconds(g_stopTime));

    UdpClientHelper client2(addr_4_from_1, port);
    client2.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client2.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client2.SetAttribute("PacketSize", UintegerValue(g_packetSize));
    ApplicationContainer app2 = client2.Install(nodes.Get(1));
    app2.Start(Seconds(1.1));
    app2.Stop(Seconds(g_stopTime));

    AnimationInterface anim("mesh_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); 

    anim.SetConstantPosition(nodes.Get(0), 50.0, 50.0); // Center
    anim.SetConstantPosition(nodes.Get(1), 20.0, 20.0); // Top Left
    anim.SetConstantPosition(nodes.Get(2), 80.0, 20.0); // Top Right
    anim.SetConstantPosition(nodes.Get(3), 20.0, 80.0); // Bottom Left
    anim.SetConstantPosition(nodes.Get(4), 80.0, 80.0); // Bottom Right

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Mesh Topology");
    Simulator::Destroy();
    return metrics;
}

// 5. Hybrid Topology
TopologyMetrics runHybridTopology() {
    std::cout << CYAN << "[5/5] Running Hybrid Topology..." << RESET << std::endl;
    NodeContainer busNodes;
    busNodes.Create(3); // Nodes 0, 1, 2
    
    NodeContainer p2pNodes;
    p2pNodes.Add(busNodes.Get(2)); // Node 2 is gateway
    p2pNodes.Create(2); // Nodes 3, 4

    NodeContainer pureP2PNodes;
    pureP2PNodes.Add(p2pNodes.Get(1)); // Node 3
    pureP2PNodes.Add(p2pNodes.Get(2)); // Node 4

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(busNodes);
    mobility.Install(pureP2PNodes);

    InternetStackHelper stack;
    stack.Install(busNodes);
    stack.Install(pureP2PNodes);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("10Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    NetDeviceContainer busDevices = csma.Install(busNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.5.1.0", "255.255.255.0");
    address.Assign(busDevices);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    NodeContainer link1(p2pNodes.Get(0), p2pNodes.Get(1)); // 2 to 3
    NetDeviceContainer p2pDev1 = p2p.Install(link1);
    address.SetBase("10.5.2.0", "255.255.255.0");
    address.Assign(p2pDev1);

    NodeContainer link2(p2pNodes.Get(0), p2pNodes.Get(2)); // 2 to 4
    NetDeviceContainer p2pDev2 = p2p.Install(link2);
    address.SetBase("10.5.3.0", "255.255.255.0");
    Ipv4InterfaceContainer targetInterfaces = address.Assign(p2pDev2);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(p2pNodes.Get(2)); // Node 4
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    UdpClientHelper client(destAddr, port);
    client.SetAttribute("MaxPackets", UintegerValue(g_maxPackets));
    client.SetAttribute("Interval", TimeValue(Seconds(g_interval)));
    client.SetAttribute("PacketSize", UintegerValue(g_packetSize));

    ApplicationContainer client1 = client.Install(busNodes.Get(0));
    client1.Start(Seconds(1.0));
    client1.Stop(Seconds(g_stopTime));

    ApplicationContainer client2 = client.Install(busNodes.Get(1));
    client2.Start(Seconds(1.1));
    client2.Stop(Seconds(g_stopTime));

    AnimationInterface anim("hybrid_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); 

    anim.SetConstantPosition(busNodes.Get(0), 20.0, 50.0);
    anim.SetConstantPosition(busNodes.Get(1), 40.0, 50.0);
    anim.SetConstantPosition(busNodes.Get(2), 60.0, 50.0); // Gateway
    anim.SetConstantPosition(p2pNodes.Get(1), 80.0, 30.0); // Node 3
    anim.SetConstantPosition(p2pNodes.Get(2), 80.0, 70.0); // Node 4

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Hybrid Topology");
    Simulator::Destroy();
    return metrics;
}

void printResults(std::vector<TopologyMetrics>& results) {
    std::cout << "\n" << BOLD << BLUE << "=================================================================================================" << RESET << "\n";
    std::cout << BOLD << "                             INTELLIGENT TOPOLOGY PERFORMANCE RESULTS                            " << RESET << "\n";
    std::cout << BOLD << BLUE << "=================================================================================================" << RESET << "\n";
    
    std::cout << BOLD << std::left 
              << std::setw(22) << "Topology" 
              << std::setw(15) << "Score"
              << std::setw(18) << "Throughput" 
              << std::setw(15) << "Delay" 
              << std::setw(15) << "Loss (%)" 
              << std::setw(10) << "Tx Pkts" << RESET << "\n";
    std::cout << "-------------------------------------------------------------------------------------------------\n";

    TopologyMetrics best = results[0];
    
    for (const auto& r : results) {
        if (r.score > best.score) best = r;
        
        std::ostringstream tp, dl, pl;
        tp << std::fixed << std::setprecision(2) << r.throughput << " Mbps";
        dl << std::fixed << std::setprecision(2) << r.delay << " ms";
        pl << std::fixed << std::setprecision(2) << r.packetLoss << " %";

        std::cout << std::left 
                  << std::setw(22) << r.name 
                  << std::setw(15) << std::fixed << std::setprecision(2) << r.score 
                  << std::setw(18) << tp.str()
                  << std::setw(15) << dl.str()
                  << std::setw(15) << pl.str()
                  << std::setw(10) << r.txPackets << "\n";
    }
    
    std::cout << BOLD << BLUE << "=================================================================================================" << RESET << "\n\n";

    std::cout << BOLD << GREEN << ">>> AUTOMATIC DECISION: THE BEST TOPOLOGY IS [" << best.name << "] <<<" << RESET << "\n";
    std::cout << YELLOW << "Reasoning: " << RESET;
    std::cout << "It achieved the highest computed score (" << best.score << ") using the weighted formula.\n";
    std::cout << "This means it successfully balanced high throughput (" << best.throughput << " Mbps) "
              << "with minimal delay (" << best.delay << " ms) and packet loss (" << best.packetLoss << "%).\n\n";

    std::ofstream outFile("results.txt");
    if (outFile.is_open()) {
        outFile << "Intelligent Topology Performance Analyzer Results\n";
        outFile << "=================================================\n";
        for (const auto& r : results) {
            outFile << r.name << " | Score: " << r.score 
                    << " | Throughput: " << r.throughput << " Mbps"
                    << " | Delay: " << r.delay << " ms"
                    << " | Loss: " << r.packetLoss << " %\n";
        }
        outFile << "\nBEST TOPOLOGY: " << best.name << "\n";
        outFile.close();
        std::cout << CYAN << "[INFO] Detailed results have been successfully saved to 'results.txt'." << RESET << "\n";
    }
}

int main(int argc, char *argv[]) {
    Time::SetResolution(NanoSeconds(1));
    LogComponentEnable("IntelligentTopologyAnalyzer", LOG_LEVEL_INFO);

    std::cout << BOLD << YELLOW << "\nInitializing Optimized Intelligent Network Topology Performance Analyzer...\n" << RESET << std::endl;

    std::vector<TopologyMetrics> results;
    results.push_back(runPointToPoint());
    results.push_back(runBusTopology());
    results.push_back(runStarTopology());
    results.push_back(runMeshTopology());
    results.push_back(runHybridTopology());

    printResults(results);

    return 0;
}

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

// ANSI Escape Codes for Professional Terminal Colors
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define CYAN    "\033[36m"
#define BOLD    "\033[1m"

// Global configuration
double g_throughputWeight = 10.0;
double g_delayWeight = 1.0;
double g_packetLossWeight = 50.0;
double g_stopTime = 10.0;

// Metric Data Structure
struct TopologyMetrics {
    std::string name;
    double throughput; // Mbps
    double delay;      // ms
    double packetLoss; // %
    double score;
    uint64_t txPackets;
    uint64_t rxPackets;
};

// Smart Calculation Logic
void calculateScore(TopologyMetrics& metrics) {
    metrics.score = (g_throughputWeight * metrics.throughput) 
                  - (g_delayWeight * metrics.delay) 
                  - (g_packetLossWeight * metrics.packetLoss);
}

// FlowMonitor Parsing
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
        if (t.destinationPort == 9) { // Filter only UDP app traffic
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

    double duration = (g_stopTime - 1.0) - 2.0; // 7 seconds active duration
    metrics.throughput = (totalRxBytes * 8.0) / (duration * 1000000.0);
    metrics.delay = (totalRxPackets > 0) ? (totalDelay / totalRxPackets) * 1000.0 : 0.0;
    metrics.packetLoss = (totalTxPackets > 0) ? ((totalTxPackets - totalRxPackets) / (double)totalTxPackets) * 100.0 : 0.0;
    
    calculateScore(metrics);
    return metrics;
}

// 1. Point-to-Point Topology
TopologyMetrics runPointToPoint() {
    std::cout << CYAN << "[1/5] Running Point-to-Point Topology (Daisy Chain)..." << RESET << std::endl;
    NodeContainer nodes;
    nodes.Create(5);

    // --- MOBILITY & POSITIONING ---
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
    for (int i = 0; i < 5; ++i) {
        positionAlloc->Add (Vector (10.0 + (i * 20.0), 50.0, 0.0)); // Horizontal Line
    }
    mobility.SetPositionAllocator (positionAlloc);
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility.Install (nodes);
    // ------------------------------

    InternetStackHelper stack;
    stack.Install(nodes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
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
        if (i == 3) {
            targetInterfaces = ifaces; 
        }
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(nodes.Get(4));
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    OnOffHelper onoff1("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff1.SetConstantRate(DataRate("60Mbps"));
    onoff1.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client1 = onoff1.Install(nodes.Get(0));
    client1.Start(Seconds(2.0));
    client1.Stop(Seconds(g_stopTime - 1.0));

    OnOffHelper onoff2("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff2.SetConstantRate(DataRate("60Mbps"));
    onoff2.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client2 = onoff2.Install(nodes.Get(1));
    client2.Start(Seconds(2.5));
    client2.Stop(Seconds(g_stopTime - 1.0));

    AnimationInterface anim("p2p_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); // Fix NetAnim trace size warning

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

    // --- MOBILITY & POSITIONING ---
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
    for (int i = 0; i < 5; ++i) {
        positionAlloc->Add (Vector (10.0 + (i * 20.0), 50.0, 0.0)); // Horizontal Line
    }
    mobility.SetPositionAllocator (positionAlloc);
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility.Install (nodes);
    // ------------------------------

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));

    NetDeviceContainer devices = csma.Install(nodes);
    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper address;
    address.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(nodes.Get(4));
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = interfaces.GetAddress(4);

    OnOffHelper onoff1("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff1.SetConstantRate(DataRate("60Mbps"));
    onoff1.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client1 = onoff1.Install(nodes.Get(0));
    client1.Start(Seconds(2.0));
    client1.Stop(Seconds(g_stopTime - 1.0));

    OnOffHelper onoff2("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff2.SetConstantRate(DataRate("60Mbps"));
    onoff2.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client2 = onoff2.Install(nodes.Get(1));
    client2.Start(Seconds(2.5));
    client2.Stop(Seconds(g_stopTime - 1.0));

    AnimationInterface anim("bus_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); // Fix NetAnim trace size warning

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

    // --- MOBILITY & POSITIONING ---
    MobilityHelper mobilityHub;
    Ptr<ListPositionAllocator> posHub = CreateObject<ListPositionAllocator> ();
    posHub->Add (Vector (50.0, 50.0, 0.0)); // Center
    mobilityHub.SetPositionAllocator (posHub);
    mobilityHub.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobilityHub.Install (hub);

    MobilityHelper mobilitySpokes;
    Ptr<ListPositionAllocator> posSpokes = CreateObject<ListPositionAllocator> ();
    posSpokes->Add (Vector (50.0, 20.0, 0.0)); // Top
    posSpokes->Add (Vector (80.0, 50.0, 0.0)); // Right
    posSpokes->Add (Vector (50.0, 80.0, 0.0)); // Bottom
    posSpokes->Add (Vector (20.0, 50.0, 0.0)); // Left
    mobilitySpokes.SetPositionAllocator (posSpokes);
    mobilitySpokes.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobilitySpokes.Install (spokes);
    // ------------------------------

    InternetStackHelper stack;
    stack.Install(hub);
    stack.Install(spokes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
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
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    OnOffHelper onoff1("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff1.SetConstantRate(DataRate("60Mbps"));
    onoff1.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client1 = onoff1.Install(spokes.Get(0));
    client1.Start(Seconds(2.0));
    client1.Stop(Seconds(g_stopTime - 1.0));

    OnOffHelper onoff2("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff2.SetConstantRate(DataRate("60Mbps"));
    onoff2.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client2 = onoff2.Install(spokes.Get(1));
    client2.Start(Seconds(2.5));
    client2.Stop(Seconds(g_stopTime - 1.0));

    AnimationInterface anim("star_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); // Fix NetAnim trace size warning

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

    // --- MOBILITY & POSITIONING ---
    MobilityHelper mobility;
    Ptr<ListPositionAllocator> positionAlloc = CreateObject<ListPositionAllocator> ();
    positionAlloc->Add (Vector (50.0, 50.0, 0.0)); // Center Node 0
    positionAlloc->Add (Vector (20.0, 20.0, 0.0)); // Top Left Node 1
    positionAlloc->Add (Vector (80.0, 20.0, 0.0)); // Top Right Node 2
    positionAlloc->Add (Vector (20.0, 80.0, 0.0)); // Bottom Left Node 3
    positionAlloc->Add (Vector (80.0, 80.0, 0.0)); // Bottom Right Node 4
    mobility.SetPositionAllocator (positionAlloc);
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility.Install (nodes);
    // ------------------------------

    InternetStackHelper stack;
    stack.Install(nodes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
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
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(g_stopTime));

    OnOffHelper onoff1("ns3::UdpSocketFactory", InetSocketAddress(addr_4_from_0, port));
    onoff1.SetConstantRate(DataRate("60Mbps"));
    onoff1.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client1 = onoff1.Install(nodes.Get(0));
    client1.Start(Seconds(2.0));
    client1.Stop(Seconds(g_stopTime - 1.0));

    OnOffHelper onoff2("ns3::UdpSocketFactory", InetSocketAddress(addr_4_from_1, port));
    onoff2.SetConstantRate(DataRate("60Mbps"));
    onoff2.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client2 = onoff2.Install(nodes.Get(1));
    client2.Start(Seconds(2.5));
    client2.Stop(Seconds(g_stopTime - 1.0));

    AnimationInterface anim("mesh_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); // Fix NetAnim trace size warning

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
    busNodes.Create(3); // 0, 1, 2
    
    NodeContainer p2pNodes;
    p2pNodes.Add(busNodes.Get(2)); // Node 2 is the gateway
    p2pNodes.Create(2); // Create Nodes 3, 4

    // --- MOBILITY & POSITIONING ---
    MobilityHelper mobilityBus;
    Ptr<ListPositionAllocator> posBus = CreateObject<ListPositionAllocator> ();
    posBus->Add (Vector (20.0, 50.0, 0.0)); // Bus Node 0
    posBus->Add (Vector (40.0, 50.0, 0.0)); // Bus Node 1
    posBus->Add (Vector (60.0, 50.0, 0.0)); // Bus Node 2 (Gateway)
    mobilityBus.SetPositionAllocator (posBus);
    mobilityBus.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobilityBus.Install (busNodes);

    MobilityHelper mobilityP2P;
    Ptr<ListPositionAllocator> posP2P = CreateObject<ListPositionAllocator> ();
    posP2P->Add (Vector (80.0, 30.0, 0.0)); // P2P Node 3 (Top Right)
    posP2P->Add (Vector (80.0, 70.0, 0.0)); // P2P Node 4 (Bottom Right)
    mobilityP2P.SetPositionAllocator (posP2P);
    mobilityP2P.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    
    NodeContainer pureP2PNodes;
    pureP2PNodes.Add(p2pNodes.Get(1)); // Node 3
    pureP2PNodes.Add(p2pNodes.Get(2)); // Node 4
    mobilityP2P.Install (pureP2PNodes);
    // ------------------------------

    InternetStackHelper stack;
    stack.Install(busNodes);
    stack.Install(pureP2PNodes);

    // Bus setup
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(NanoSeconds(6560)));
    NetDeviceContainer busDevices = csma.Install(busNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.5.1.0", "255.255.255.0");
    address.Assign(busDevices);

    // P2P setup: Node 2 to Node 3, Node 2 to Node 4
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
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

    // Traffic: Bus Node 0 and Node 1 sending to P2P Node 4
    uint16_t port = 9;
    PacketSinkHelper sink("ns3::UdpSocketFactory", InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer serverApp = sink.Install(p2pNodes.Get(2)); // Node 4
    serverApp.Start(Seconds(1.0));
    serverApp.Stop(Seconds(g_stopTime));

    Ipv4Address destAddr = targetInterfaces.GetAddress(1);

    OnOffHelper onoff1("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff1.SetConstantRate(DataRate("60Mbps"));
    onoff1.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client1 = onoff1.Install(busNodes.Get(0));
    client1.Start(Seconds(2.0));
    client1.Stop(Seconds(g_stopTime - 1.0));

    OnOffHelper onoff2("ns3::UdpSocketFactory", InetSocketAddress(destAddr, port));
    onoff2.SetConstantRate(DataRate("60Mbps"));
    onoff2.SetAttribute("PacketSize", UintegerValue(1024));
    ApplicationContainer client2 = onoff2.Install(busNodes.Get(1));
    client2.Start(Seconds(2.5));
    client2.Stop(Seconds(g_stopTime - 1.0));

    AnimationInterface anim("hybrid_anim.xml");
    anim.SetMaxPktsPerTraceFile(5000000); // Fix NetAnim trace size warning

    FlowMonitorHelper flowmon;
    Ptr<FlowMonitor> monitor = flowmon.InstallAll();

    Simulator::Stop(Seconds(g_stopTime));
    Simulator::Run();

    TopologyMetrics metrics = parseMetrics(monitor, flowmon, "Hybrid Topology");
    Simulator::Destroy();
    return metrics;
}

// 6. Print Results
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

    // Write to results.txt
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
    } else {
        std::cerr << RED << "[ERROR] Unable to open results.txt for writing." << RESET << "\n";
    }
}

int main(int argc, char *argv[]) {
    Time::SetResolution(NanoSeconds(1));
    LogComponentEnable("IntelligentTopologyAnalyzer", LOG_LEVEL_INFO);

    std::cout << BOLD << YELLOW << "\nInitializing Intelligent Network Topology Performance Analyzer...\n" << RESET << std::endl;

    std::vector<TopologyMetrics> results;
    results.push_back(runPointToPoint());
    results.push_back(runBusTopology());
    results.push_back(runStarTopology());
    results.push_back(runMeshTopology());
    results.push_back(runHybridTopology());

    printResults(results);

    return 0;
}

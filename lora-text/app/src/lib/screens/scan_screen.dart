import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:provider/provider.dart';

import '../services/ble_service.dart';
import 'chat_screen.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen({super.key});

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  @override
  Widget build(BuildContext context) {
    final ble = context.watch<BleService>();

    return Scaffold(
      appBar: AppBar(
        title: const Text('LoRa Text'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        actions: [
          if (ble.isScanning)
            const Padding(
              padding: EdgeInsets.all(14),
              child: SizedBox(
                width: 20, height: 20,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
        ],
      ),
      body: Column(
        children: [
          // Status banner
          _StatusBanner(status: ble.status),

          // Device list
          Expanded(
            child: ble.scanResults.isEmpty
                ? Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        const Icon(Icons.bluetooth_searching, size: 64,
                            color: Colors.grey),
                        const SizedBox(height: 16),
                        Text(
                          ble.isScanning
                              ? 'Scanning for LoRaText devices...'
                              : 'Tap Scan to find nearby devices',
                          style: Theme.of(context).textTheme.bodyLarge,
                        ),
                      ],
                    ),
                  )
                : ListView.builder(
                    itemCount: ble.scanResults.length,
                    itemBuilder: (ctx, i) =>
                        _DeviceTile(result: ble.scanResults[i]),
                  ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: ble.isScanning
            ? () => ble.stopScan()
            : () => ble.startScan(),
        icon: Icon(ble.isScanning ? Icons.stop : Icons.search),
        label: Text(ble.isScanning ? 'Stop' : 'Scan'),
      ),
    );
  }
}

class _StatusBanner extends StatelessWidget {
  final BleStatus status;
  const _StatusBanner({required this.status});

  @override
  Widget build(BuildContext context) {
    Color color;
    String text;
    switch (status) {
      case BleStatus.connected:
        color = Colors.green.shade100;
        text  = '● Connected';
        break;
      case BleStatus.connecting:
        color = Colors.orange.shade100;
        text  = '◌ Connecting...';
        break;
      case BleStatus.scanning:
        color = Colors.blue.shade100;
        text  = '◎ Scanning...';
        break;
      default:
        color = Colors.grey.shade100;
        text  = '○ Disconnected';
    }
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      color: color,
      child: Text(text, style: const TextStyle(fontWeight: FontWeight.w600)),
    );
  }
}

class _DeviceTile extends StatelessWidget {
  final ScanResult result;
  const _DeviceTile({required this.result});

  @override
  Widget build(BuildContext context) {
    final ble    = context.read<BleService>();
    final name   = result.device.platformName;
    final rssi   = result.rssi;

    return ListTile(
      leading: const Icon(Icons.bluetooth, color: Colors.indigo),
      title: Text(name, style: const TextStyle(fontWeight: FontWeight.bold)),
      subtitle: Text('RSSI: $rssi dBm  •  ${result.device.remoteId}'),
      trailing: ElevatedButton(
        onPressed: () => _connect(context, ble, result.device),
        child: const Text('Connect'),
      ),
    );
  }

  Future<void> _connect(
      BuildContext context, BleService ble, BluetoothDevice device) async {
    await ble.connect(device);
    if (context.mounted && ble.isConnected) {
      Navigator.of(context).push(
        MaterialPageRoute(builder: (_) => const ChatScreen()),
      );
    }
  }
}

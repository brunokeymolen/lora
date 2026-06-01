import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'services/ble_service.dart';
import 'screens/scan_screen.dart';

void main() {
  runApp(
    ChangeNotifierProvider(
      create: (_) => BleService(),
      child: const LoRaTextApp(),
    ),
  );
}

class LoRaTextApp extends StatelessWidget {
  const LoRaTextApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LoRa Text',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.indigo,
          brightness: Brightness.light,
        ),
        useMaterial3: true,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.indigo,
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: const ScanScreen(),
    );
  }
}

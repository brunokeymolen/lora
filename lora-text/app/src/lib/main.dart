/*
 * TrailText
 * Text when networks fail.
 *
 * Copyright (c) 2026 Bruno Keymolen
 *
 * This work is licensed under the Creative Commons
 * Attribution-NonCommercial-ShareAlike 4.0 International License.
 *
 * You are free to share and adapt this work for non-commercial purposes,
 * provided that appropriate credit is given and any derivative works are
 * distributed under the same license.
 *
 * License: CC BY-NC-SA 4.0
 * See: https://creativecommons.org/licenses/by-nc-sa/4.0/
 */

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
      title: 'TrailText',
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

import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../models/message.dart';

/// Nordic UART Service UUIDs (used by nRF Connect, Serial Terminal, etc.)
const _nusSvcUuid  = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const _nusTxUuid   = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // phone → ESP32
const _nusRxUuid   = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // ESP32 → phone (notify)

enum BleStatus { idle, scanning, connecting, connected, disconnected }

class BleService extends ChangeNotifier {
  BleStatus _status = BleStatus.idle;
  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar; // we write to this
  BluetoothCharacteristic? _rxChar; // we subscribe to this

  final List<ScanResult> _scanResults = [];
  final List<LoRaMessage> _messages   = [];

  String _connectedName = '';
  String _ownMac = '';
  StreamSubscription? _rxSub;
  StreamSubscription? _stateSub;

  // ── Public getters ─────────────────────────────────────────────────────────

  BleStatus get status  => _status;
  bool get isConnected  => _status == BleStatus.connected;
  bool get isScanning   => _status == BleStatus.scanning;

  List<ScanResult>   get scanResults => List.unmodifiable(_scanResults);
  List<LoRaMessage>  get messages    => List.unmodifiable(_messages);
  String get connectedName           => _connectedName;

  // ── Scanning ───────────────────────────────────────────────────────────────

  Future<void> startScan() async {
    _scanResults.clear();
    _setStatus(BleStatus.scanning);

    FlutterBluePlus.scanResults.listen((results) {
      _scanResults
        ..clear()
        ..addAll(results
            .where((r) => r.device.platformName.startsWith('LoRaText-'))
            .toList()
          ..sort((a, b) => b.rssi.compareTo(a.rssi)));
      notifyListeners();
    });

    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 10),
      withServices: [Guid(_nusSvcUuid)],
    );

    await Future.delayed(const Duration(seconds: 10));
    if (_status == BleStatus.scanning) _setStatus(BleStatus.idle);
  }

  Future<void> stopScan() async {
    await FlutterBluePlus.stopScan();
    if (_status == BleStatus.scanning) _setStatus(BleStatus.idle);
  }

  // ── Connection ─────────────────────────────────────────────────────────────

  Future<void> connect(BluetoothDevice device) async {
    await stopScan();
    _setStatus(BleStatus.connecting);
    _connectedName = device.platformName;
    notifyListeners();

    try {
      await device.connect(timeout: const Duration(seconds: 15));
      _device = device;

      // Monitor connection state
      _stateSub = device.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected) {
          _handleDisconnect();
        }
      });

      // Request larger MTU for longer messages
      await device.requestMtu(247);

      // Discover services
      final services = await device.discoverServices();
      for (final svc in services) {
        if (svc.uuid.toString().toLowerCase() == _nusSvcUuid) {
          for (final chr in svc.characteristics) {
            final uuid = chr.uuid.toString().toLowerCase();
            if (uuid == _nusTxUuid) _txChar = chr;
            if (uuid == _nusRxUuid) _rxChar = chr;
          }
        }
      }

      if (_txChar == null || _rxChar == null) {
        throw Exception('NUS characteristics not found');
      }

      // Subscribe to RX notifications
      await _rxChar!.setNotifyValue(true);
      _rxSub = _rxChar!.onValueReceived.listen(_onNotification);

      // Extract own MAC from device name: "LoRaText-AABBCC" → 3 byte MAC
      final nameParts = device.platformName.split('-');
      _ownMac = nameParts.length > 1 ? nameParts.last : '';

      _setStatus(BleStatus.connected);
    } catch (e) {
      debugPrint('BLE connect error: $e');
      _handleDisconnect();
    }
  }

  Future<void> disconnect() async {
    await _device?.disconnect();
    _handleDisconnect();
  }

  void _handleDisconnect() {
    _rxSub?.cancel();
    _stateSub?.cancel();
    _rxSub = null;
    _stateSub = null;
    _txChar = null;
    _rxChar = null;
    _device = null;
    _setStatus(BleStatus.disconnected);
  }

  // ── Send message ───────────────────────────────────────────────────────────

  /// Send a text message via BLE → ESP32 → LoRa (encrypted on ESP32).
  /// Returns true on success.
  Future<bool> sendMessage(String text) async {
    if (!isConnected || _txChar == null || text.isEmpty) return false;

    // Truncate to 180 bytes (firmware limit)
    final bytes = utf8.encode(text);
    final payload = bytes.length > 180 ? bytes.sublist(0, 180) : bytes;

    try {
      await _txChar!.write(payload, withoutResponse: true);

      // Add as our own outgoing message
      _messages.add(LoRaMessage(
        from: _ownMac,
        text: utf8.decode(payload),
        rssi: 0,
        snr: 0,
        timestamp: DateTime.now(),
        isOwn: true,
      ));
      notifyListeners();
      return true;
    } catch (e) {
      debugPrint('BLE write error: $e');
      return false;
    }
  }

  // ── Incoming notification ──────────────────────────────────────────────────

  void _onNotification(List<int> value) {
    try {
      final json = jsonDecode(utf8.decode(value)) as Map<String, dynamic>;
      final msg = LoRaMessage(
        from:      json['from']  as String? ?? '??????',
        text:      json['text']  as String? ?? '',
        rssi:     (json['rssi'] as num?)?.toInt() ?? 0,
        snr:      (json['snr']  as num?)?.toInt() ?? 0,
        timestamp: DateTime.now(),
        isOwn:     false,
      );
      _messages.add(msg);
      notifyListeners();
    } catch (e) {
      debugPrint('BLE RX parse error: $e  raw: ${utf8.decode(value)}');
    }
  }

  // ── Helpers ────────────────────────────────────────────────────────────────

  void clearMessages() {
    _messages.clear();
    notifyListeners();
  }

  void _setStatus(BleStatus s) {
    _status = s;
    notifyListeners();
  }
}

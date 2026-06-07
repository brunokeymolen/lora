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

import 'dart:async';
import 'dart:convert';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:flutter/widgets.dart';

import '../models/message.dart';

/// Nordic UART Service UUIDs (used by nRF Connect, Serial Terminal, etc.)
const _nusSvcUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const _nusTxUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // phone → ESP32
const _nusRxUuid =
    '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // ESP32 → phone (notify)
const _protoMsgPrefix = '@M|';
const _protoLocPrefix = '@L|';
const _protoAckPrefix = '@A|';
const _firmwarePayloadLimit = 180;
const _storedMessagesKey = 'stored_messages_v1';
const _maxStoredMessages = 200;
const _deviceNamePrefixes = ['loratext-', 'trailtext-'];

enum BleStatus { idle, scanning, connecting, connected, disconnected }

const _messageNotificationChannel = AndroidNotificationChannel(
  'lora_messages',
  'LoRa Messages',
  description: 'Incoming LoRa messages',
  importance: Importance.high,
  playSound: true,
);

class BleService extends ChangeNotifier {
  BleStatus _status = BleStatus.idle;
  BluetoothDevice? _device;
  BluetoothCharacteristic? _txChar; // we write to this
  BluetoothCharacteristic? _rxChar; // we subscribe to this

  final List<ScanResult> _scanResults = [];
  final List<LoRaMessage> _messages = [];

  String _connectedName = '';
  String _ownMac = '';
  StreamSubscription? _rxSub;
  StreamSubscription? _stateSub;
  final Map<String, int> _ownMessageIndexById = {};
  int _messageSeq = 0;
  final FlutterLocalNotificationsPlugin _notifications =
      FlutterLocalNotificationsPlugin();
  late final AppLifecycleListener _lifecycleListener;
  AppLifecycleState _appLifecycleState = AppLifecycleState.resumed;

  BleService() {
    _lifecycleListener = AppLifecycleListener(
      onStateChange: (state) => _appLifecycleState = state,
    );
    unawaited(_initializeNotifications());
    unawaited(_restoreMessages());
  }

  // ── Public getters ─────────────────────────────────────────────────────────

  BleStatus get status => _status;
  bool get isConnected => _status == BleStatus.connected;
  bool get isScanning => _status == BleStatus.scanning;

  List<ScanResult> get scanResults => List.unmodifiable(_scanResults);
  List<LoRaMessage> get messages => List.unmodifiable(_messages);
  String get connectedName => _connectedName;

  // ── Scanning ───────────────────────────────────────────────────────────────

  Future<void> startScan() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
      Permission.notification,
    ].request();

    final scanGranted = await Permission.bluetoothScan.isGranted;
    if (!scanGranted) {
      debugPrint('BLE scan permission denied');
      return;
    }

    _scanResults.clear();
    _setStatus(BleStatus.scanning);

    FlutterBluePlus.scanResults.listen((results) {
      final filtered = results.where(_isTargetDevice).toList();
      final visible = filtered.isNotEmpty ? filtered : results;
      _scanResults
        ..clear()
        ..addAll(visible..sort((a, b) => b.rssi.compareTo(a.rssi)));
      notifyListeners();
    });

    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 10),
    );

    await Future.delayed(const Duration(seconds: 10));
    if (_status == BleStatus.scanning) _setStatus(BleStatus.idle);
  }

  Future<void> stopScan() async {
    await FlutterBluePlus.stopScan();
    if (_status == BleStatus.scanning) _setStatus(BleStatus.idle);
  }

  bool _isTargetDevice(ScanResult result) {
    final platformName = result.device.platformName.toLowerCase();
    final advertisedName = result.advertisementData.advName.toLowerCase();
    final nameMatch = _deviceNamePrefixes.any((prefix) {
      return platformName.startsWith(prefix) ||
          advertisedName.startsWith(prefix);
    });

    final hasNusService = result.advertisementData.serviceUuids.any((uuid) {
      return uuid.toString().toLowerCase() == _nusSvcUuid;
    });

    return nameMatch || hasNusService;
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
    final trimmed = text.trim();
    if (trimmed.isEmpty) return false;
    return _sendPacket(type: 'msg', text: trimmed);
  }

  Future<bool> sendLocation(double latitude, double longitude) {
    return _sendPacket(
      type: 'loc',
      text: '${latitude.toStringAsFixed(6)}, ${longitude.toStringAsFixed(6)}',
      latitude: latitude,
      longitude: longitude,
    );
  }

  // ── Incoming notification ──────────────────────────────────────────────────

  void _onNotification(List<int> value) {
    try {
      final json = jsonDecode(utf8.decode(value)) as Map<String, dynamic>;
      final from = json['from'] as String? ?? '??????';
      final wireText = json['text'] as String? ?? '';
      final rssi = (json['rssi'] as num?)?.toInt() ?? 0;
      final snr = (json['snr'] as num?)?.toInt() ?? 0;

      if (_handleAckPacket(wireText)) {
        return;
      }

      final parsed = _parseIncomingPacket(wireText);
      final messageId = parsed?['id'] as String?;
      final kind = parsed?['kind'] as String?;
      final text = parsed?['text'] as String? ?? wireText;
      final lat = parsed?['latitude'] as double?;
      final lon = parsed?['longitude'] as double?;

      final msg = LoRaMessage(
        from: from,
        text: text,
        rssi: rssi,
        snr: snr,
        timestamp: DateTime.now(),
        isOwn: false,
        messageId: messageId,
        latitude: lat,
        longitude: lon,
      );
      _appendMessage(msg);
      unawaited(_showIncomingMessageNotification(msg));

      if (messageId != null && (kind == 'msg' || kind == 'loc')) {
        unawaited(_sendAck(messageId));
      }
    } catch (e) {
      debugPrint('BLE RX parse error: $e  raw: ${utf8.decode(value)}');
    }
  }

  // ── Helpers ────────────────────────────────────────────────────────────────

  void clearMessages() {
    _messages.clear();
    _ownMessageIndexById.clear();
    notifyListeners();
    unawaited(_persistMessages());
  }

  @override
  void dispose() {
    _lifecycleListener.dispose();
    super.dispose();
  }

  void _setStatus(BleStatus s) {
    _status = s;
    notifyListeners();
  }

  Future<void> _initializeNotifications() async {
    const initSettings = InitializationSettings(
      android: AndroidInitializationSettings('@mipmap/ic_launcher'),
    );

    await _notifications.initialize(initSettings);

    final androidNotifications =
        _notifications.resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>();

    await androidNotifications?.createNotificationChannel(
      _messageNotificationChannel,
    );
    await androidNotifications?.requestNotificationsPermission();
  }

  Future<void> _showIncomingMessageNotification(LoRaMessage msg) async {
    if (_appLifecycleState == AppLifecycleState.resumed) {
      return;
    }

    final androidDetails = AndroidNotificationDetails(
      _messageNotificationChannel.id,
      _messageNotificationChannel.name,
      channelDescription: _messageNotificationChannel.description,
      importance: Importance.high,
      priority: Priority.high,
      playSound: true,
      category: AndroidNotificationCategory.message,
    );

    await _notifications.show(
      msg.timestamp.millisecondsSinceEpoch ~/ 1000,
      'LoRa message from ${msg.from}',
      msg.text,
      NotificationDetails(android: androidDetails),
    );
  }

  Future<bool> _sendPacket({
    required String type,
    required String text,
    double? latitude,
    double? longitude,
  }) async {
    if (!isConnected || _txChar == null) return false;

    final id = _nextMessageId();
    final wire = _encodeWirePayload(
      type: type,
      id: id,
      text: text,
      latitude: latitude,
      longitude: longitude,
    );
    final payload = utf8.encode(wire);
    if (payload.length > _firmwarePayloadLimit) {
      debugPrint('Payload too long: ${payload.length} bytes');
      return false;
    }

    try {
      await _txChar!.write(payload, withoutResponse: true);
      final ownMsg = LoRaMessage(
        from: _ownMac,
        text: text,
        rssi: 0,
        snr: 0,
        timestamp: DateTime.now(),
        isOwn: true,
        messageId: id,
        isAcked: false,
        latitude: latitude,
        longitude: longitude,
      );
      _appendMessage(ownMsg);
      return true;
    } catch (e) {
      debugPrint('BLE write error: $e');
      return false;
    }
  }

  Future<void> _sendAck(String messageId) async {
    if (!isConnected || _txChar == null) return;
    final payload = utf8.encode('$_protoAckPrefix$messageId');
    if (payload.length > _firmwarePayloadLimit) return;
    try {
      await _txChar!.write(payload, withoutResponse: true);
    } catch (e) {
      debugPrint('BLE ack write error: $e');
    }
  }

  bool _handleAckPacket(String wireText) {
    if (!wireText.startsWith(_protoAckPrefix)) {
      return false;
    }
    final ackedId = wireText.substring(_protoAckPrefix.length).trim();
    if (ackedId.isEmpty) return true;
    _markOwnMessageAcked(ackedId);
    return true;
  }

  Map<String, Object?>? _parseIncomingPacket(String wireText) {
    if (wireText.startsWith(_protoMsgPrefix)) {
      final parts = wireText.split('|');
      if (parts.length < 3) return null;
      return {
        'kind': 'msg',
        'id': parts[1],
        'text': parts.sublist(2).join('|'),
      };
    }

    if (wireText.startsWith(_protoLocPrefix)) {
      final parts = wireText.split('|');
      if (parts.length < 4) return null;
      final lat = double.tryParse(parts[2]);
      final lon = double.tryParse(parts[3]);
      if (lat == null || lon == null) return null;
      return {
        'kind': 'loc',
        'id': parts[1],
        'text': '${lat.toStringAsFixed(6)}, ${lon.toStringAsFixed(6)}',
        'latitude': lat,
        'longitude': lon,
      };
    }

    return null;
  }

  String _encodeWirePayload({
    required String type,
    required String id,
    required String text,
    double? latitude,
    double? longitude,
  }) {
    if (type == 'loc' && latitude != null && longitude != null) {
      return '$_protoLocPrefix$id|${latitude.toStringAsFixed(6)}|${longitude.toStringAsFixed(6)}';
    }
    return '$_protoMsgPrefix$id|$text';
  }

  String _nextMessageId() {
    _messageSeq = (_messageSeq + 1) % 1000;
    return '${DateTime.now().millisecondsSinceEpoch}_$_messageSeq';
  }

  void _markOwnMessageAcked(String messageId) {
    int? idx = _ownMessageIndexById[messageId];
    if (idx == null || idx < 0 || idx >= _messages.length) {
      idx = _messages.lastIndexWhere(
        (m) => m.isOwn && m.messageId == messageId,
      );
      if (idx < 0) return;
      _ownMessageIndexById[messageId] = idx;
    }

    final current = _messages[idx];
    if (current.isAcked) return;
    _messages[idx] = current.copyWith(
      isAcked: true,
      ackedAt: DateTime.now(),
    );
    notifyListeners();
    unawaited(_persistMessages());
  }

  void _appendMessage(LoRaMessage message) {
    _messages.add(message);
    _trimMessages();
    _rebuildOwnMessageIndex();
    notifyListeners();
    unawaited(_persistMessages());
  }

  void _trimMessages() {
    final overflow = _messages.length - _maxStoredMessages;
    if (overflow > 0) {
      _messages.removeRange(0, overflow);
    }
  }

  void _rebuildOwnMessageIndex() {
    _ownMessageIndexById
      ..clear()
      ..addEntries(
        _messages.indexed.where((entry) {
          final message = entry.$2;
          return message.isOwn && message.messageId != null;
        }).map(
          (entry) => MapEntry(entry.$2.messageId!, entry.$1),
        ),
      );
  }

  Future<void> _restoreMessages() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final stored = prefs.getString(_storedMessagesKey);
      if (stored == null || stored.isEmpty) return;

      final decoded = jsonDecode(stored);
      if (decoded is! List) return;

      _messages
        ..clear()
        ..addAll(
          decoded.whereType<Map>().map(
                (item) => LoRaMessage.fromJson(Map<String, dynamic>.from(item)),
              ),
        );
      _trimMessages();
      _rebuildOwnMessageIndex();
      notifyListeners();
    } catch (e) {
      debugPrint('Restore messages failed: $e');
    }
  }

  Future<void> _persistMessages() async {
    try {
      final prefs = await SharedPreferences.getInstance();
      final encoded = jsonEncode(
        _messages.map((message) => message.toJson()).toList(growable: false),
      );
      await prefs.setString(_storedMessagesKey, encoded);
    } catch (e) {
      debugPrint('Persist messages failed: $e');
    }
  }
}

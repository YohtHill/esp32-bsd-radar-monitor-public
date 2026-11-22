package com.example.bsdmonitor;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

public class MainActivity extends Activity {
    private static final String TARGET_NAME = "ESP32_BSD_MONITOR";
    private static final String ALT_TARGET_NAME = "ESP32_BSD_Radar";
    private static final UUID SPP_UUID =
            UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int MAX_TARGET_ROWS = 8;

    private static final int COLOR_BG = Color.rgb(244, 248, 252);
    private static final int COLOR_SURFACE = Color.rgb(255, 255, 255);
    private static final int COLOR_SURFACE_BLUE = Color.rgb(232, 244, 255);
    private static final int COLOR_TEXT = Color.rgb(25, 45, 64);
    private static final int COLOR_MUTED = Color.rgb(104, 121, 139);
    private static final int COLOR_LINE = Color.rgb(219, 229, 238);
    private static final int COLOR_GREEN = Color.rgb(0, 168, 132);
    private static final int COLOR_BLUE = Color.rgb(47, 128, 237);
    private static final int COLOR_CYAN = Color.rgb(24, 176, 206);
    private static final int COLOR_YELLOW = Color.rgb(244, 181, 49);
    private static final int COLOR_ORANGE = Color.rgb(239, 119, 42);
    private static final int COLOR_RED = Color.rgb(225, 62, 74);

    private BluetoothAdapter bluetoothAdapter;
    private BluetoothSocket socket;
    private OutputStream outputStream;
    private Thread readerThread;
    private volatile boolean readerRunning;

    private Spinner deviceSpinner;
    private ArrayAdapter<String> deviceAdapter;
    private final List<BluetoothDevice> pairedDevices = new ArrayList<>();
    private final List<View> levelSegments = new ArrayList<>();
    private final List<TargetSnapshot> targetSnapshots = new ArrayList<>();

    private TextView connectionView;
    private TextView stateBannerView;
    private TextView messageView;
    private TextView leftZoneView;
    private TextView centerZoneView;
    private TextView rightZoneView;
    private TextView targetSummaryView;
    private LinearLayout targetListView;
    private TextView logView;

    private Button muteButton;
    private Button alarmButton;
    private Button falseButton;
    private Button resetButton;

    private RadarCanvasView radarView;

    private String lastState = "SAFE";
    private String lastRisk = "OK";
    private String lastZone = "N";
    private String lastDistance = "0.00";
    private String lastSpeed = "0.0";
    private String lastAngle = "0";
    private String lastCollision = "OK";
    private String lastBuzzer = "OFF";
    private String lastMessage = "";
    private int lastLevel = 0;
    private int lastTargetCount = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        bluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
        setContentView(buildUi());
        ensurePermissions();
        refreshPairedDevices();
        updateDashboard();
    }

    @Override
    protected void onDestroy() {
        disconnect();
        super.onDestroy();
    }

    private View buildUi() {
        ScrollView scrollRoot = new ScrollView(this);
        scrollRoot.setFillViewport(true);
        scrollRoot.setBackgroundColor(COLOR_BG);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(18), dp(18), dp(20));
        scrollRoot.addView(root, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));

        root.addView(buildHeader(), matchWrapNoMargin());
        root.addView(buildConnectionPanel(), matchWrap());
        root.addView(buildMonitorPanel(), matchWrap());
        root.addView(buildTargetsPanel(), matchWrap());
        root.addView(buildCommandPanel(), matchWrap());
        root.addView(buildLogPanel(), matchWrap());
        return scrollRoot;
    }

    private View buildHeader() {
        LinearLayout header = row(Gravity.CENTER_VERTICAL);
        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);

        TextView title = text("盲区雷达助手", 28, COLOR_TEXT, true);
        TextView subtitle = text("BSD 车侧安全监测", 13, COLOR_MUTED, false);
        titleCol.addView(title, matchWrapNoMargin());
        titleCol.addView(subtitle, matchWrapNoMargin());
        header.addView(titleCol, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));

        connectionView = text("未连接", 12, COLOR_RED, true);
        connectionView.setGravity(Gravity.CENTER);
        connectionView.setPadding(dp(12), dp(7), dp(12), dp(7));
        connectionView.setBackground(chip(Color.rgb(255, 236, 238), COLOR_RED));
        header.addView(connectionView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        return header;
    }

    private View buildConnectionPanel() {
        LinearLayout panel = panel();
        TextView label = sectionTitle("设备连接");
        panel.addView(label, matchWrapNoMargin());

        LinearLayout pickerRow = row(Gravity.CENTER_VERTICAL);
        pickerRow.setPadding(dp(10), dp(6), dp(8), dp(6));
        pickerRow.setBackground(roundedStroke(COLOR_SURFACE_BLUE, COLOR_LINE, dp(12), dp(1)));

        TextView pickerLabel = text("设备", 12, COLOR_MUTED, true);
        pickerLabel.setGravity(Gravity.CENTER_VERTICAL);
        pickerRow.addView(pickerLabel, new LinearLayout.LayoutParams(
                dp(42), LinearLayout.LayoutParams.WRAP_CONTENT));

        deviceSpinner = new Spinner(this);
        deviceSpinner.setPadding(dp(6), 0, 0, 0);
        deviceSpinner.setMinimumHeight(dp(38));
        deviceSpinner.setBackgroundColor(Color.TRANSPARENT);
        pickerRow.addView(deviceSpinner, new LinearLayout.LayoutParams(
                0, dp(42), 1.0f));

        LinearLayout.LayoutParams pickerParams = matchWrapNoMargin();
        pickerParams.setMargins(0, dp(12), 0, dp(10));
        panel.addView(pickerRow, pickerParams);

        LinearLayout connectRow = row(Gravity.CENTER);
        connectRow.addView(button("刷新设备", COLOR_CYAN, v -> refreshPairedDevices()), rowWeightParams());
        connectRow.addView(button("连接", COLOR_GREEN, v -> connectSelected()), rowWeightParams());
        connectRow.addView(button("断开", Color.rgb(121, 137, 153), v -> disconnect()), rowWeightParams());
        panel.addView(connectRow, matchWrapNoMargin());
        return panel;
    }

    private View buildMonitorPanel() {
        LinearLayout panel = panel();
        stateBannerView = text("", 22, Color.WHITE, true);
        stateBannerView.setGravity(Gravity.CENTER);
        stateBannerView.setPadding(dp(10), dp(13), dp(10), dp(13));
        panel.addView(stateBannerView, matchWrapNoMargin());

        radarView = new RadarCanvasView(this);
        LinearLayout.LayoutParams radarParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(190));
        radarParams.setMargins(0, dp(8), 0, dp(8));
        panel.addView(radarView, radarParams);

        LinearLayout zoneRow = row(Gravity.CENTER);
        leftZoneView = zoneCard("左侧");
        centerZoneView = zoneCard("中间");
        rightZoneView = zoneCard("右侧");
        zoneRow.addView(leftZoneView, rowWeightParams());
        zoneRow.addView(centerZoneView, rowWeightParams());
        zoneRow.addView(rightZoneView, rowWeightParams());
        panel.addView(zoneRow, matchWrapNoMargin());

        LinearLayout levelRow = row(Gravity.CENTER_VERTICAL);
        TextView levelLabel = text("风险等级", 13, COLOR_MUTED, true);
        levelRow.addView(levelLabel, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        LinearLayout levelBar = row(Gravity.CENTER);
        LinearLayout.LayoutParams levelBarParams = new LinearLayout.LayoutParams(
                0, dp(14), 1.0f);
        levelBarParams.setMargins(dp(12), 0, 0, 0);
        for (int i = 0; i < 5; i++) {
            View segment = new View(this);
            LinearLayout.LayoutParams segmentParams = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.MATCH_PARENT, 1.0f);
            segmentParams.setMargins(dp(3), 0, dp(3), 0);
            levelBar.addView(segment, segmentParams);
            levelSegments.add(segment);
        }
        levelRow.addView(levelBar, levelBarParams);
        panel.addView(levelRow, matchWrap());

        messageView = text("", 14, COLOR_MUTED, false);
        messageView.setGravity(Gravity.CENTER);
        panel.addView(messageView, matchWrapNoMargin());
        return panel;
    }

    private View buildTargetsPanel() {
        LinearLayout panel = panel();

        LinearLayout titleRow = row(Gravity.CENTER_VERTICAL);
        titleRow.addView(sectionTitle("目标监测"), new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        targetSummaryView = text("暂无目标", 13, COLOR_MUTED, true);
        targetSummaryView.setGravity(Gravity.CENTER);
        targetSummaryView.setPadding(dp(10), dp(5), dp(10), dp(5));
        targetSummaryView.setBackground(chip(Color.rgb(239, 246, 252), COLOR_LINE));
        titleRow.addView(targetSummaryView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        panel.addView(titleRow, matchWrapNoMargin());

        LinearLayout headerRow = row(Gravity.CENTER_VERTICAL);
        headerRow.setPadding(dp(6), dp(10), dp(6), dp(4));
        headerRow.addView(targetColumnHeader("目标", 62), fixedHeaderParams(62));
        headerRow.addView(targetColumnHeader("速度", 0), new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        headerRow.addView(targetColumnHeader("距离", 0), new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        headerRow.addView(targetColumnHeader("角度", 0), new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        headerRow.addView(targetColumnHeader("危险", 0), new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        panel.addView(headerRow, matchWrapNoMargin());

        targetListView = new LinearLayout(this);
        targetListView.setOrientation(LinearLayout.VERTICAL);
        panel.addView(targetListView, matchWrapNoMargin());
        return panel;
    }

    private View buildCommandPanel() {
        LinearLayout panel = panel();
        panel.addView(sectionTitle("处置操作"), matchWrapNoMargin());

        LinearLayout row1 = row(Gravity.CENTER);
        muteButton = button("静音", Color.rgb(121, 137, 153), v -> sendCommand("MUTE"));
        alarmButton = button("紧急报警", COLOR_RED, v -> sendCommand("ALARM"));
        row1.addView(muteButton, rowWeightParams());
        row1.addView(alarmButton, rowWeightParams());
        panel.addView(row1, matchWrap());

        LinearLayout row2 = row(Gravity.CENTER);
        falseButton = button("误报确认", COLOR_YELLOW, v -> sendCommand("FALSE"));
        resetButton = button("恢复监测", COLOR_BLUE, v -> sendCommand("RESET"));
        row2.addView(falseButton, rowWeightParams());
        row2.addView(resetButton, rowWeightParams());
        panel.addView(row2, matchWrapNoMargin());
        return panel;
    }

    private View buildLogPanel() {
        LinearLayout panel = panel();
        panel.addView(sectionTitle("通讯记录"), matchWrapNoMargin());
        logView = text("", 11, Color.rgb(84, 100, 116), false);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setMaxLines(8);
        logView.setPadding(dp(10), dp(10), dp(10), dp(10));
        logView.setBackground(roundedStroke(Color.rgb(247, 250, 253), COLOR_LINE, dp(10), dp(1)));
        panel.addView(logView, matchWrap());
        return panel;
    }

    private LinearLayout panel() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(14), dp(14), dp(14), dp(14));
        panel.setBackground(roundedStroke(COLOR_SURFACE, COLOR_LINE, dp(16), dp(1)));
        panel.setElevation(dp(2));
        return panel;
    }

    private TextView sectionTitle(String title) {
        return text(title, 16, COLOR_TEXT, true);
    }

    private TextView text(String text, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setIncludeFontPadding(true);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        }
        return view;
    }

    private TextView zoneCard(String label) {
        TextView view = text(label + "\n安全", 14, COLOR_MUTED, true);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(6), dp(11), dp(6), dp(11));
        view.setBackground(roundedStroke(Color.rgb(247, 250, 253), COLOR_LINE, dp(12), dp(1)));
        return view;
    }

    private TextView targetColumnHeader(String label, int widthDp) {
        TextView view = text(label, 11, COLOR_MUTED, true);
        view.setGravity(Gravity.CENTER);
        if (widthDp > 0) {
            view.setMinWidth(dp(widthDp));
        }
        return view;
    }

    private LinearLayout.LayoutParams fixedHeaderParams(int widthDp) {
        return new LinearLayout.LayoutParams(dp(widthDp),
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private View targetRow(TargetSnapshot target) {
        LinearLayout card = row(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(6), dp(8), dp(6), dp(8));
        card.setBackground(roundedStroke(Color.rgb(247, 250, 253),
                Color.rgb(225, 234, 242), dp(12), dp(1)));

        TextView name = text("目标" + target.number + "\n" + zoneName(target.zone),
                12, COLOR_TEXT, true);
        name.setGravity(Gravity.CENTER);
        card.addView(name, new LinearLayout.LayoutParams(dp(62),
                LinearLayout.LayoutParams.WRAP_CONTENT));

        card.addView(targetCell(formatSpeed(target.speed), COLOR_TEXT), rowTargetParams());
        card.addView(targetCell(formatDistance(target.distance), COLOR_TEXT), rowTargetParams());
        card.addView(targetCell(formatAngle(target.angle), COLOR_TEXT), rowTargetParams());
        card.addView(targetCell(target.level + "/5", levelColor(target.level)), rowTargetParams());

        LinearLayout.LayoutParams params = matchWrapNoMargin();
        params.setMargins(0, dp(3), 0, dp(3));
        card.setLayoutParams(params);
        return card;
    }

    private TextView targetCell(String value, int color) {
        TextView view = text(value, 12, color, true);
        view.setGravity(Gravity.CENTER);
        view.setSingleLine(false);
        return view;
    }

    private LinearLayout.LayoutParams rowTargetParams() {
        return new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
    }

    private View emptyTargetRow() {
        TextView empty = text("暂无目标，持续监测中", 14, COLOR_MUTED, false);
        empty.setGravity(Gravity.CENTER);
        empty.setPadding(dp(10), dp(14), dp(10), dp(14));
        empty.setBackground(roundedStroke(Color.rgb(247, 250, 253),
                COLOR_LINE, dp(12), dp(1)));
        return empty;
    }

    private Button button(String text, int color, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(13);
        button.setTextColor(contrastColor(color));
        button.setAllCaps(false);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setMinHeight(dp(44));
        button.setPadding(dp(6), 0, dp(6), 0);
        button.setBackground(rounded(color, dp(12)));
        button.setOnClickListener(listener);
        return button;
    }

    private LinearLayout row(int gravity) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(gravity);
        return row;
    }

    private LinearLayout.LayoutParams rowWeightParams() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        params.setMargins(dp(4), dp(4), dp(4), dp(4));
        return params;
    }

    private LinearLayout.LayoutParams matchWrap() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, dp(8), 0, dp(8));
        return params;
    }

    private LinearLayout.LayoutParams matchWrapNoMargin() {
        return new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(radius);
        return drawable;
    }

    private GradientDrawable roundedStroke(int color, int strokeColor, int radius, int strokeWidth) {
        GradientDrawable drawable = rounded(color, radius);
        drawable.setStroke(strokeWidth, strokeColor);
        return drawable;
    }

    private GradientDrawable roundedGradient(int startColor, int endColor, int radius) {
        GradientDrawable drawable = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{startColor, endColor});
        drawable.setCornerRadius(radius);
        return drawable;
    }

    private GradientDrawable chip(int color, int strokeColor) {
        return roundedStroke(color, strokeColor, dp(18), dp(1));
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private int contrastColor(int bg) {
        int r = Color.red(bg);
        int g = Color.green(bg);
        int b = Color.blue(bg);
        int luminance = (r * 299 + g * 587 + b * 114) / 1000;
        return luminance > 150 ? COLOR_TEXT : Color.WHITE;
    }

    private void ensurePermissions() {
        if (Build.VERSION.SDK_INT >= 31 &&
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                        != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT}, 100);
        } else if (Build.VERSION.SDK_INT <= 30 &&
                checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                        != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, 101);
        }
    }

    private boolean hasBluetoothPermission() {
        return Build.VERSION.SDK_INT < 31 ||
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                        == PackageManager.PERMISSION_GRANTED;
    }

    private void refreshPairedDevices() {
        pairedDevices.clear();
        List<String> names = new ArrayList<>();

        if (bluetoothAdapter == null) {
            names.add("本机不支持蓝牙");
        } else if (!hasBluetoothPermission()) {
            names.add("需要蓝牙权限");
        } else {
            try {
                Set<BluetoothDevice> bonded = bluetoothAdapter.getBondedDevices();
                BluetoothDevice target = null;
                for (BluetoothDevice device : bonded) {
                    String name = device.getName();
                    if (TextUtils.isEmpty(name)) {
                        name = device.getAddress();
                    }
                    if (TARGET_NAME.equals(name) || ALT_TARGET_NAME.equals(name)) {
                        target = device;
                    } else {
                        pairedDevices.add(device);
                        names.add(name + "  " + device.getAddress());
                    }
                }
                if (target != null) {
                    pairedDevices.add(0, target);
                    names.add(0, safeDeviceName(target) + "  " + target.getAddress());
                }
                if (names.isEmpty()) {
                    names.add("暂无已配对设备");
                }
            } catch (SecurityException ex) {
                names.add("蓝牙权限被拒绝");
            }
        }

        deviceAdapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, names) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                return styleDeviceSpinnerItem(super.getView(position, convertView, parent), false);
            }

            @Override
            public View getDropDownView(int position, View convertView, ViewGroup parent) {
                return styleDeviceSpinnerItem(super.getDropDownView(position, convertView, parent), true);
            }
        };
        deviceAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        deviceSpinner.setAdapter(deviceAdapter);
    }

    private View styleDeviceSpinnerItem(View view, boolean dropdown) {
        if (view instanceof TextView) {
            TextView textView = (TextView) view;
            textView.setTextColor(COLOR_TEXT);
            textView.setTextSize(13);
            textView.setSingleLine(true);
            textView.setEllipsize(TextUtils.TruncateAt.END);
            textView.setGravity(Gravity.CENTER_VERTICAL);
            textView.setPadding(dp(8), 0, dp(8), 0);
            textView.setBackgroundColor(dropdown ? COLOR_SURFACE : Color.TRANSPARENT);
        }
        return view;
    }

    private void connectSelected() {
        if (bluetoothAdapter == null || pairedDevices.isEmpty() || !hasBluetoothPermission()) {
            toast("请先在系统蓝牙中配对 " + TARGET_NAME);
            return;
        }

        int index = deviceSpinner.getSelectedItemPosition();
        if (index < 0 || index >= pairedDevices.size()) {
            toast("没有选择蓝牙设备");
            return;
        }

        BluetoothDevice device = pairedDevices.get(index);
        disconnect();
        setConnectionStatus("连接中...", false);

        new Thread(() -> {
            try {
                BluetoothSocket newSocket =
                        device.createRfcommSocketToServiceRecord(SPP_UUID);
                bluetoothAdapter.cancelDiscovery();
                newSocket.connect();
                socket = newSocket;
                outputStream = socket.getOutputStream();
                runOnUiThread(() -> setConnectionStatus("已连接 " + safeDeviceName(device), true));
                startReader(socket);
            } catch (IOException | SecurityException ex) {
                closeSocketQuietly(socket);
                socket = null;
                outputStream = null;
                runOnUiThread(() -> {
                    setConnectionStatus("未连接", false);
                    toast("连接失败：" + ex.getMessage());
                });
            }
        }).start();
    }

    private void setConnectionStatus(String text, boolean connected) {
        if (connectionView == null) {
            return;
        }
        connectionView.setText(text);
        connectionView.setTextColor(connected ? COLOR_GREEN : COLOR_RED);
        connectionView.setBackground(chip(
                connected ? Color.rgb(229, 249, 243) : Color.rgb(255, 236, 238),
                connected ? COLOR_GREEN : COLOR_RED));
    }

    private String safeDeviceName(BluetoothDevice device) {
        try {
            String name = device.getName();
            return TextUtils.isEmpty(name) ? device.getAddress() : name;
        } catch (SecurityException ex) {
            return "蓝牙设备";
        }
    }

    private void startReader(BluetoothSocket connectedSocket) {
        readerRunning = true;
        readerThread = new Thread(() -> {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    connectedSocket.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while (readerRunning && (line = reader.readLine()) != null) {
                    String finalLine = line;
                    runOnUiThread(() -> handleStatusLine(finalLine));
                }
            } catch (IOException ex) {
                if (readerRunning) {
                    runOnUiThread(() -> {
                        setConnectionStatus("未连接", false);
                        appendLog("接收中断：" + ex.getMessage());
                    });
                }
            }
        });
        readerThread.start();
    }

    private void disconnect() {
        readerRunning = false;
        closeSocketQuietly(socket);
        socket = null;
        outputStream = null;
        setConnectionStatus("未连接", false);
    }

    private void closeSocketQuietly(BluetoothSocket target) {
        if (target == null) {
            return;
        }
        try {
            target.close();
        } catch (IOException ignored) {
        }
    }

    private void sendCommand(String command) {
        OutputStream out = outputStream;
        if (out == null) {
            toast("尚未连接设备");
            return;
        }
        try {
            out.write((command + "\n").getBytes(StandardCharsets.UTF_8));
            out.flush();
            appendLog("发送 " + command);
        } catch (IOException ex) {
            toast("发送失败：" + ex.getMessage());
        }
    }

    private void handleStatusLine(String line) {
        appendLog("收到 " + line);
        Map<String, String> fields = parseFields(line);

        lastState = valueOr(fields.get("STATE"), lastState);
        lastRisk = valueOr(fields.get("RISK"), lastRisk);
        lastZone = valueOr(fields.get("ZONE"), lastZone);
        lastDistance = valueOr(fields.get("DIST"), lastDistance);
        lastSpeed = valueOr(fields.get("SPEED"), lastSpeed);
        lastAngle = valueOr(fields.get("ANGLE"), lastAngle);
        lastCollision = valueOr(fields.get("COLLISION"), lastCollision);
        lastBuzzer = valueOr(fields.get("BUZZER"), lastBuzzer);
        lastMessage = valueOr(fields.get("MSG"), lastMessage);

        String level = fields.get("LEVEL");
        if (level != null) {
            try {
                lastLevel = Math.max(0, Math.min(5, Integer.parseInt(level)));
            } catch (NumberFormatException ignored) {
                lastLevel = 0;
            }
        }

        String count = fields.get("CNT");
        if (count != null) {
            try {
                lastTargetCount = Math.max(0, Integer.parseInt(count));
            } catch (NumberFormatException ignored) {
                lastTargetCount = 0;
            }
        }

        updateTargetSnapshots(fields);
        updateDashboard();
    }

    private String valueOr(String value, String fallback) {
        return value == null ? fallback : value;
    }

    private void updateDashboard() {
        int accent = colorForStatus(lastState, lastRisk, lastCollision, lastLevel);
        stateBannerView.setText(bannerText(lastState));
        stateBannerView.setTextColor(contrastColor(accent));
        stateBannerView.setBackground(roundedGradient(lighten(accent), accent, dp(14)));

        messageView.setText(messageForState(lastState, lastMessage));
        messageView.setTextColor(accent == COLOR_YELLOW ? COLOR_TEXT : COLOR_MUTED);

        updateZoneCards(accent);
        updateLevelBar();
        updateTargetList();

        radarView.setStatus(lastState, lastZone, lastLevel,
                parseFloat(lastDistance), parseFloat(lastAngle), lastTargetCount);

        boolean confirmState = "COLLISION_CONFIRM".equals(lastState);
        setButtonEmphasis(alarmButton, confirmState ? COLOR_RED : Color.rgb(146, 158, 172));
        setButtonEmphasis(falseButton, confirmState ? COLOR_YELLOW : Color.rgb(146, 158, 172));
        setButtonEmphasis(resetButton, ("ALARMING".equals(lastState) || "FALSE_ALARM".equals(lastState))
                ? COLOR_BLUE : Color.rgb(146, 158, 172));
    }

    private void updateTargetSnapshots(Map<String, String> fields) {
        targetSnapshots.clear();
        int count = Math.min(MAX_TARGET_ROWS, Math.max(0, lastTargetCount));
        for (int i = 0; i < count; i++) {
            String prefix = "T" + i + "_";
            if (!fields.containsKey(prefix + "DIST") && !fields.containsKey(prefix + "ID")) {
                continue;
            }

            TargetSnapshot target = new TargetSnapshot();
            target.number = i + 1;
            target.id = valueOr(fields.get(prefix + "ID"), String.valueOf(target.number));
            target.zone = valueOr(fields.get(prefix + "ZONE"), "N");
            target.distance = valueOr(fields.get(prefix + "DIST"), "0.00");
            target.speed = valueOr(fields.get(prefix + "SPEED"), "0.0");
            target.angle = valueOr(fields.get(prefix + "ANGLE"), "0");
            target.level = parseLevel(valueOr(fields.get(prefix + "LEVEL"), "0"));
            targetSnapshots.add(target);
        }

        if (targetSnapshots.isEmpty() && lastTargetCount > 0 && !"N".equals(lastZone)) {
            TargetSnapshot target = new TargetSnapshot();
            target.number = 1;
            target.id = valueOr(fields.get("ID"), "1");
            target.zone = lastZone;
            target.distance = lastDistance;
            target.speed = lastSpeed;
            target.angle = lastAngle;
            target.level = lastLevel;
            targetSnapshots.add(target);
        }
    }

    private int parseLevel(String value) {
        try {
            return Math.max(0, Math.min(5, Integer.parseInt(value)));
        } catch (NumberFormatException ex) {
            return 0;
        }
    }

    private void updateTargetList() {
        if (targetSummaryView == null || targetListView == null) {
            return;
        }

        int accent = colorForStatus(lastState, lastRisk, lastCollision, lastLevel);
        if (lastTargetCount > 0) {
            targetSummaryView.setText("检测到 " + lastTargetCount + " 个目标");
            targetSummaryView.setTextColor(contrastColor(accent));
            targetSummaryView.setBackground(chip(lighten(accent), accent));
        } else {
            targetSummaryView.setText("暂无目标");
            targetSummaryView.setTextColor(COLOR_MUTED);
            targetSummaryView.setBackground(chip(Color.rgb(239, 246, 252), COLOR_LINE));
        }

        targetListView.removeAllViews();
        if (targetSnapshots.isEmpty()) {
            targetListView.addView(emptyTargetRow(), matchWrapNoMargin());
            return;
        }

        for (TargetSnapshot target : targetSnapshots) {
            targetListView.addView(targetRow(target));
        }
    }

    private void setButtonEmphasis(Button button, int color) {
        button.setTextColor(contrastColor(color));
        button.setBackground(rounded(color, dp(12)));
    }

    private void updateZoneCards(int accent) {
        setZoneCard(leftZoneView, "左侧", "L".equals(lastZone), accent);
        setZoneCard(centerZoneView, "中间", "C".equals(lastZone), accent);
        setZoneCard(rightZoneView, "右侧", "R".equals(lastZone), accent);
    }

    private void setZoneCard(TextView view, String label, boolean active, int accent) {
        String state = active ? ("等级 " + lastLevel) : "安全";
        int bg = active ? accent : Color.rgb(247, 250, 253);
        view.setText(label + "\n" + state);
        view.setTextColor(active ? contrastColor(bg) : COLOR_MUTED);
        view.setBackground(roundedStroke(bg,
                active ? Color.WHITE : COLOR_LINE, dp(12), dp(1)));
    }

    private void updateLevelBar() {
        int color = levelColor(lastLevel);
        for (int i = 0; i < levelSegments.size(); i++) {
            int segmentColor = i < lastLevel ? color : Color.rgb(226, 234, 241);
            levelSegments.get(i).setBackground(rounded(segmentColor, dp(8)));
        }
    }

    private String bannerText(String state) {
        if ("COLLISION_CONFIRM".equals(state)) {
            return "检测到碰撞";
        }
        if ("ALARMING".equals(state)) {
            return "正在报警中";
        }
        if ("FALSE_ALARM".equals(state)) {
            return "误报已确认";
        }
        if ("WARNING".equals(state)) {
            return "盲区预警";
        }
        return "系统安全";
    }

    private String messageForState(String state, String message) {
        if (!TextUtils.isEmpty(message)) {
            if ("CONFIRM_REQUIRED".equals(message)) {
                return "请确认是否需要触发紧急报警";
            }
            if ("EMERGENCY_REPORTING".equals(message)) {
                return "紧急报警流程已启动，蜂鸣器已静音";
            }
            if ("FALSE_ALARM_CONFIRMED".equals(message)) {
                return "误报已确认，系统即将恢复监测";
            }
            return message.replace('_', ' ');
        }
        if ("COLLISION_CONFIRM".equals(state)) {
            return "请根据现场情况选择紧急报警或误报确认";
        }
        if ("ALARMING".equals(state)) {
            return "紧急报警流程已启动，蜂鸣器已静音";
        }
        if ("FALSE_ALARM".equals(state)) {
            return "误报已确认，系统即将恢复监测";
        }
        if ("WARNING".equals(state)) {
            return zoneName(lastZone) + "存在目标，请注意侧后方";
        }
        return "盲区区域清晰，持续监测中";
    }

    private Map<String, String> parseFields(String line) {
        Map<String, String> fields = new HashMap<>();
        String[] parts = line.split(",");
        for (String part : parts) {
            int index = part.indexOf(':');
            if (index <= 0) {
                continue;
            }
            fields.put(part.substring(0, index).trim(), part.substring(index + 1).trim());
        }
        return fields;
    }

    private String zoneName(String zone) {
        if ("L".equals(zone)) return "左侧";
        if ("C".equals(zone)) return "中间";
        if ("R".equals(zone)) return "右侧";
        return "无目标";
    }

    private String formatDistance(String value) {
        return value + " m";
    }

    private String formatSpeed(String value) {
        return value + " km/h";
    }

    private String formatAngle(String value) {
        return value + "°";
    }

    private int colorForStatus(String state, String risk, String collision, int level) {
        if ("HIT".equals(collision) || "COLLISION_CONFIRM".equals(state)) {
            return COLOR_RED;
        }
        if ("ALARMING".equals(state)) {
            return COLOR_ORANGE;
        }
        if ("FALSE_ALARM".equals(state)) {
            return COLOR_GREEN;
        }
        if ("WARNING".equals(state) || "WARN".equals(risk) || level > 0) {
            return levelColor(level);
        }
        return COLOR_GREEN;
    }

    private int levelColor(int level) {
        if (level >= 5) return COLOR_RED;
        if (level == 4) return COLOR_ORANGE;
        if (level == 3) return COLOR_YELLOW;
        if (level >= 1) return COLOR_BLUE;
        return COLOR_GREEN;
    }

    private int lighten(int color) {
        return Color.rgb(
                Math.min(255, Color.red(color) + 42),
                Math.min(255, Color.green(color) + 42),
                Math.min(255, Color.blue(color) + 42));
    }

    private float parseFloat(String value) {
        if (value == null) {
            return 0.0f;
        }
        try {
            return Float.parseFloat(value);
        } catch (NumberFormatException ex) {
            return 0.0f;
        }
    }

    private void appendLog(String text) {
        if (logView == null) {
            return;
        }
        String old = logView.getText().toString();
        String next = old + text + "\n";
        if (next.length() > 3500) {
            next = next.substring(next.length() - 3500);
        }
        logView.setText(next);
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    private static class TargetSnapshot {
        int number;
        String id;
        String zone;
        String distance;
        String speed;
        String angle;
        int level;
    }

    private static class RadarCanvasView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();
        private final RectF rect = new RectF();

        private String state = "SAFE";
        private String zone = "N";
        private int level = 0;
        private int targetCount = 0;
        private float distanceM = 0.0f;
        private float angleDeg = 0.0f;

        RadarCanvasView(Activity activity) {
            super(activity);
            setLayerType(View.LAYER_TYPE_SOFTWARE, null);
        }

        void setStatus(String state, String zone, int level,
                       float distanceM, float angleDeg, int targetCount) {
            this.state = state == null ? "SAFE" : state;
            this.zone = zone == null ? "N" : zone;
            this.level = Math.max(0, Math.min(5, level));
            this.distanceM = Math.max(0.0f, Math.abs(distanceM));
            this.angleDeg = angleDeg;
            this.targetCount = Math.max(0, targetCount);
            invalidate();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            int w = getWidth();
            int h = getHeight();
            float cx = w / 2.0f;
            float originY = h - dp(18.0f);
            float maxR = Math.min(w * 0.50f, h * 0.96f);

            drawBackground(canvas, w, h);
            drawRadarField(canvas, w, h, cx, originY, maxR);
            drawDynamicSweep(canvas, cx, originY, maxR);
            drawVehicle(canvas, cx, originY);
            drawTarget(canvas, cx, originY, maxR);
            drawLabels(canvas, w, h);

            postInvalidateOnAnimation();
        }

        private void drawBackground(Canvas canvas, int w, int h) {
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new LinearGradient(0, 0, w, h,
                    Color.rgb(238, 247, 255), Color.rgb(248, 252, 255),
                    Shader.TileMode.CLAMP));
            rect.set(0, 0, w, h);
            canvas.drawRoundRect(rect, 28, 28, paint);
            paint.setShader(null);
        }

        private void drawRadarField(Canvas canvas, int w, int h,
                                    float cx, float originY, float maxR) {
            int activeColor = levelColorForCanvas(level);
            int baseColor = "SAFE".equals(state) ? COLOR_GREEN : activeColor;

            paint.setStyle(Paint.Style.FILL);
            paint.setShader(new RadialGradient(cx, originY, maxR,
                    withAlpha(baseColor, 60), Color.TRANSPARENT,
                    Shader.TileMode.CLAMP));
            canvas.drawCircle(cx, originY, maxR, paint);
            paint.setShader(null);

            drawSector(canvas, cx, originY, maxR, -60, -15, "L");
            drawSector(canvas, cx, originY, maxR, -15, 15, "C");
            drawSector(canvas, cx, originY, maxR, 15, 60, "R");

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2);
            paint.setColor(Color.rgb(204, 222, 236));
            for (int i = 1; i <= 4; i++) {
                float r = maxR * i / 4.0f;
                rect.set(cx - r, originY - r, cx + r, originY + r);
                canvas.drawArc(rect, -150, 120, false, paint);
            }

            drawAngleLine(canvas, cx, originY, maxR, -60);
            drawAngleLine(canvas, cx, originY, maxR, -15);
            drawAngleLine(canvas, cx, originY, maxR, 15);
            drawAngleLine(canvas, cx, originY, maxR, 60);
        }

        private void drawSector(Canvas canvas, float cx, float originY, float maxR,
                                float startDeg, float endDeg, String targetZone) {
            boolean active = targetZone.equals(zone) && targetCount > 0 && !"SAFE".equals(state);
            int fill = active ? levelColorForCanvas(level) : Color.rgb(228, 238, 247);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(fill, active ? 150 : 115));
            path.reset();
            path.moveTo(cx, originY);
            addPolar(path, cx, originY, maxR, startDeg);
            addPolar(path, cx, originY, maxR, endDeg);
            path.close();
            canvas.drawPath(path, paint);

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(active ? 4 : 2);
            paint.setColor(active ? Color.WHITE : Color.rgb(198, 216, 230));
            canvas.drawPath(path, paint);
        }

        private void drawDynamicSweep(Canvas canvas, float cx, float originY, float maxR) {
            long now = SystemClock.uptimeMillis();
            float t = (now % 2200L) / 2200.0f;
            float sweepAngle = -60.0f + t * 120.0f;
            int sweepColor = "SAFE".equals(state) ? COLOR_CYAN : levelColorForCanvas(level);

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(4);
            paint.setColor(withAlpha(sweepColor, 150));
            float x = polarX(cx, maxR, sweepAngle);
            float y = polarY(originY, maxR, sweepAngle);
            canvas.drawLine(cx, originY, x, y, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(sweepColor, 26));
            path.reset();
            path.moveTo(cx, originY);
            addPolar(path, cx, originY, maxR, sweepAngle - 10);
            addPolar(path, cx, originY, maxR, sweepAngle + 10);
            path.close();
            canvas.drawPath(path, paint);

            float pulse = (now % 1600L) / 1600.0f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3);
            paint.setColor(withAlpha(sweepColor, (int) (110 * (1.0f - pulse))));
            canvas.drawCircle(cx, originY, maxR * pulse, paint);
        }

        private void drawAngleLine(Canvas canvas, float cx, float originY,
                                   float maxR, float angle) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2);
            paint.setColor(Color.rgb(199, 218, 232));
            canvas.drawLine(cx, originY, polarX(cx, maxR, angle), polarY(originY, maxR, angle), paint);
        }

        private void drawVehicle(Canvas canvas, float cx, float originY) {
            float halfW = dp(22.0f);
            float bodyTop = dp(30.0f);
            float bodyBottom = dp(12.0f);
            float windowHalfW = dp(13.0f);
            float windowTop = dp(23.0f);
            float windowBottom = dp(6.0f);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.rgb(31, 63, 90));
            rect.set(cx - halfW, originY - bodyTop, cx + halfW, originY + bodyBottom);
            canvas.drawRoundRect(rect, dp(8.0f), dp(8.0f), paint);

            paint.setColor(Color.rgb(133, 205, 255));
            rect.set(cx - windowHalfW, originY - windowTop, cx + windowHalfW, originY - windowBottom);
            canvas.drawRoundRect(rect, dp(4.0f), dp(4.0f), paint);

            paint.setColor(Color.WHITE);
            canvas.drawCircle(cx - dp(17.0f), originY + dp(12.0f), dp(4.0f), paint);
            canvas.drawCircle(cx + dp(17.0f), originY + dp(12.0f), dp(4.0f), paint);
        }

        private void drawTarget(Canvas canvas, float cx, float originY, float maxR) {
            if ("N".equals(zone) || targetCount == 0 || "SAFE".equals(state)) {
                return;
            }

            float displayAngle = angleDeg;
            if (Math.abs(displayAngle) < 0.1f) {
                if ("L".equals(zone)) {
                    displayAngle = -35.0f;
                } else if ("R".equals(zone)) {
                    displayAngle = 35.0f;
                }
            }
            displayAngle = Math.max(-58.0f, Math.min(58.0f, displayAngle));
            float r = maxR * Math.min(1.0f, Math.max(0.18f, distanceM / 30.0f));
            float x = polarX(cx, r, displayAngle);
            float y = polarY(originY, r, displayAngle);
            int color = levelColorForCanvas(level);

            long now = SystemClock.uptimeMillis();
            float pulse = (now % 900L) / 900.0f;
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3);
            paint.setColor(withAlpha(color, (int) (145 * (1.0f - pulse))));
            canvas.drawCircle(x, y, 16 + 18 * pulse, paint);

            paint.setStyle(Paint.Style.FILL);
            paint.setColor(color);
            canvas.drawCircle(x, y, 12, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(3);
            paint.setColor(Color.WHITE);
            canvas.drawCircle(x, y, 17, paint);
        }

        private void drawLabels(Canvas canvas, int w, int h) {
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
            paint.setTextSize(sp(15.0f));
            paint.setColor(COLOR_TEXT);
            canvas.drawText("左侧", w * 0.23f, dp(27.0f), paint);
            canvas.drawText("中间", w * 0.50f, dp(27.0f), paint);
            canvas.drawText("右侧", w * 0.77f, dp(27.0f), paint);

            paint.setTypeface(Typeface.DEFAULT);
            paint.setTextSize(sp(13.0f));
            paint.setColor(COLOR_MUTED);
            String distance = distanceM <= 0.0f ? "暂无目标" : String.format("%.2f m · %d 个目标", distanceM, targetCount);
            canvas.drawText(distance, w / 2.0f, h - dp(6.0f), paint);
        }

        private void addPolar(Path targetPath, float cx, float originY, float radius, float angleDeg) {
            targetPath.lineTo(polarX(cx, radius, angleDeg), polarY(originY, radius, angleDeg));
        }

        private float polarX(float cx, float radius, float angleDeg) {
            double rad = Math.toRadians(angleDeg);
            return cx + (float) Math.sin(rad) * radius;
        }

        private float polarY(float originY, float radius, float angleDeg) {
            double rad = Math.toRadians(angleDeg);
            return originY - (float) Math.cos(rad) * radius;
        }

        private int withAlpha(int color, int alpha) {
            return Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color));
        }

        private float dp(float value) {
            return value * getResources().getDisplayMetrics().density;
        }

        private float sp(float value) {
            return value * getResources().getDisplayMetrics().scaledDensity;
        }

        private int levelColorForCanvas(int level) {
            if (level >= 5) return COLOR_RED;
            if (level == 4) return COLOR_ORANGE;
            if (level == 3) return COLOR_YELLOW;
            if (level >= 1) return COLOR_BLUE;
            return COLOR_GREEN;
        }
    }
}

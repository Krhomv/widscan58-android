// The native side of assets/bridge/android-bridge.js: USB host access, speech
// and file saving for the web UI. GPL-3.0-or-later.
package io.github.krhomv.widscan58;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.hardware.usb.UsbRequest;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.speech.tts.TextToSpeech;
import android.util.Base64;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class UsbBridge {
    private static final String TAG = "widscan58";
    private static final String ACTION_PERMISSION = BuildConfig.APPLICATION_ID + ".USB_PERMISSION";
    private static final int TRANSFER_SIZE = 16384;
    private static final int TIMEOUT_MS = 1000;

    private final Activity activity;
    private final WebView web;
    private final UsbManager usb;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newCachedThreadPool();
    private final Map<String, Conn> conns = new ConcurrentHashMap<>();
    private final Map<String, Integer> permissionCallbacks = new ConcurrentHashMap<>();
    private TextToSpeech tts;
    private boolean ttsReady;
    private final List<Runnable> ttsQueue = new ArrayList<>();

    /** An open device. */
    private static final class Conn {
        final UsbDevice device;
        final UsbDeviceConnection connection;
        final Set<UsbInterface> claimed = ConcurrentHashMap.newKeySet();
        final Set<Integer> readers = ConcurrentHashMap.newKeySet();
        volatile boolean closed;

        Conn(UsbDevice device, UsbDeviceConnection connection) {
            this.device = device;
            this.connection = connection;
        }
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            UsbDevice device = deviceExtra(intent);
            if (device == null) return;
            String action = intent.getAction();
            if (ACTION_PERMISSION.equals(action)) {
                Integer cb = permissionCallbacks.remove(device.getDeviceName());
                if (cb != null) callback(cb, true, usb.hasPermission(device) ? "true" : "false");
            } else if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                attached(device);
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                detached(device);
            }
        }
    };

    UsbBridge(Activity activity, WebView web) {
        this.activity = activity;
        this.web = web;
        this.usb = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        activity.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
    }

    static UsbDevice deviceExtra(Intent intent) {
        if (Build.VERSION.SDK_INT >= 33) return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    void shutdown() {
        try {
            activity.unregisterReceiver(receiver);
        } catch (IllegalArgumentException ignored) {
            // not registered
        }
        for (String id : conns.keySet()) usbClose(id);
        io.shutdownNow();
        if (tts != null) tts.shutdown();
    }

    // ---- Events to the page ----

    private final Set<String> announced = new HashSet<>();

    /** A board was plugged in (broadcast, or the activity's attach intent). */
    void attached(UsbDevice device) {
        main.post(() -> {
            if (!announced.add(device.getDeviceName())) return;
            try {
                js("__widscan._attached(" + describe(device) + ")");
            } catch (JSONException e) {
                Log.w(TAG, "attach", e);
            }
        });
    }

    private void detached(UsbDevice device) {
        String id = device.getDeviceName();
        Conn c = conns.remove(id);
        if (c != null) closeConn(c);
        main.post(() -> {
            announced.remove(id);
            js("__widscan._detached(" + JSONObject.quote(id) + ")");
        });
    }

    private void js(String code) {
        web.evaluateJavascript("window.__widscan&&" + code, null);
    }

    private void callback(int cb, boolean ok, String resultJs) {
        main.post(() -> js("__widscan._cb(" + cb + "," + ok + "," + resultJs + ")"));
    }

    // ---- WebUSB ----

    private UsbDevice find(String id) {
        return usb.getDeviceList().get(id);
    }

    private JSONObject describe(UsbDevice d) throws JSONException {
        boolean granted = usb.hasPermission(d);
        JSONObject o = new JSONObject();
        o.put("id", d.getDeviceName());
        o.put("vendorId", d.getVendorId());
        o.put("productId", d.getProductId());
        o.put("deviceClass", d.getDeviceClass());
        o.put("deviceSubclass", d.getDeviceSubclass());
        o.put("deviceProtocol", d.getDeviceProtocol());
        o.put("productName", d.getProductName() == null ? "" : d.getProductName());
        o.put("manufacturerName", d.getManufacturerName() == null ? "" : d.getManufacturerName());
        String serial = "";
        if (granted) {
            try {
                serial = d.getSerialNumber();
            } catch (SecurityException ignored) {
                // needs permission on Android 10+
            }
        }
        o.put("serialNumber", serial == null ? "" : serial);
        o.put("granted", granted);
        JSONArray configs = new JSONArray();
        for (int ci = 0; ci < d.getConfigurationCount(); ci++) {
            UsbConfiguration c = d.getConfiguration(ci);
            JSONObject co = new JSONObject();
            co.put("configurationValue", c.getId());
            co.put("configurationName", c.getName() == null ? "" : c.getName());
            JSONArray ifaces = new JSONArray();
            JSONObject byNumber = new JSONObject();
            for (int ii = 0; ii < c.getInterfaceCount(); ii++) {
                UsbInterface i = c.getInterface(ii);
                String key = Integer.toString(i.getId());
                JSONObject io = byNumber.optJSONObject(key);
                if (io == null) {
                    io = new JSONObject();
                    io.put("interfaceNumber", i.getId());
                    io.put("alternates", new JSONArray());
                    byNumber.put(key, io);
                    ifaces.put(io);
                }
                JSONObject a = new JSONObject();
                a.put("alternateSetting", i.getAlternateSetting());
                a.put("interfaceClass", i.getInterfaceClass());
                a.put("interfaceSubclass", i.getInterfaceSubclass());
                a.put("interfaceProtocol", i.getInterfaceProtocol());
                a.put("interfaceName", i.getName() == null ? "" : i.getName());
                JSONArray eps = new JSONArray();
                for (int ei = 0; ei < i.getEndpointCount(); ei++) {
                    UsbEndpoint e = i.getEndpoint(ei);
                    JSONObject eo = new JSONObject();
                    eo.put("endpointNumber", e.getEndpointNumber());
                    eo.put("direction", e.getDirection() == UsbConstants.USB_DIR_IN ? "in" : "out");
                    eo.put("type", endpointType(e.getType()));
                    eo.put("packetSize", e.getMaxPacketSize());
                    eps.put(eo);
                }
                a.put("endpoints", eps);
                io.getJSONArray("alternates").put(a);
            }
            co.put("interfaces", ifaces);
            configs.put(co);
        }
        o.put("configurations", configs);
        return o;
    }

    private static String endpointType(int t) {
        switch (t) {
            case UsbConstants.USB_ENDPOINT_XFER_BULK:
                return "bulk";
            case UsbConstants.USB_ENDPOINT_XFER_INT:
                return "interrupt";
            case UsbConstants.USB_ENDPOINT_XFER_ISOC:
                return "isochronous";
            default:
                return "control";
        }
    }

    @JavascriptInterface
    public String usbDevices() {
        JSONArray a = new JSONArray();
        for (UsbDevice d : usb.getDeviceList().values()) {
            try {
                a.put(describe(d));
            } catch (JSONException e) {
                Log.w(TAG, "describe", e);
            }
        }
        return a.toString();
    }

    @JavascriptInterface
    public void usbRequestPermission(String id, int cb) {
        main.post(() -> {
            UsbDevice d = find(id);
            if (d == null) {
                callback(cb, true, "false");
            } else if (usb.hasPermission(d)) {
                callback(cb, true, "true");
            } else {
                permissionCallbacks.put(id, cb);
                Intent intent = new Intent(ACTION_PERMISSION).setPackage(activity.getPackageName());
                int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
                usb.requestPermission(d, PendingIntent.getBroadcast(activity, 0, intent, flags));
            }
        });
    }

    @JavascriptInterface
    public boolean usbOpen(String id) {
        if (conns.containsKey(id)) return true;
        UsbDevice d = find(id);
        if (d == null || !usb.hasPermission(d)) return false;
        UsbDeviceConnection c = usb.openDevice(d);
        if (c == null) return false;
        conns.put(id, new Conn(d, c));
        return true;
    }

    private static UsbInterface iface(Conn c, int number) {
        for (int i = 0; i < c.device.getInterfaceCount(); i++) {
            UsbInterface u = c.device.getInterface(i);
            if (u.getId() == number && u.getAlternateSetting() == 0) return u;
        }
        return null;
    }

    @JavascriptInterface
    public boolean usbClaim(String id, int number) {
        Conn c = conns.get(id);
        UsbInterface i = c == null ? null : iface(c, number);
        if (i == null || !c.connection.claimInterface(i, true)) return false;
        c.claimed.add(i);
        return true;
    }

    @JavascriptInterface
    public void usbRelease(String id, int number) {
        Conn c = conns.get(id);
        UsbInterface i = c == null ? null : iface(c, number);
        if (i == null) return;
        c.connection.releaseInterface(i);
        c.claimed.remove(i);
    }

    @JavascriptInterface
    public void usbClose(String id) {
        Conn c = conns.remove(id);
        if (c != null) closeConn(c);
    }

    private static void closeConn(Conn c) {
        c.closed = true;
        for (UsbInterface i : c.claimed) c.connection.releaseInterface(i);
        c.claimed.clear();
        c.connection.close(); // also ends the readers' requestWait()
    }

    private static UsbEndpoint endpoint(Conn c, int number, int direction) {
        for (UsbInterface i : c.claimed) {
            for (int e = 0; e < i.getEndpointCount(); e++) {
                UsbEndpoint ep = i.getEndpoint(e);
                if (ep.getEndpointNumber() == number && ep.getDirection() == direction) return ep;
            }
        }
        return null;
    }

    /** Starts reading an IN endpoint until the device closes; the bytes go to
     *  the page in batches (one evaluateJavascript per main-thread turn). */
    @JavascriptInterface
    public boolean usbStartIn(String id, int number) {
        Conn c = conns.get(id);
        UsbEndpoint ep = c == null ? null : endpoint(c, number, UsbConstants.USB_DIR_IN);
        if (ep == null) return false;
        if (!c.readers.add(number)) return true;
        io.execute(() -> read(id, c, ep));
        return true;
    }

    private void read(String id, Conn c, UsbEndpoint ep) {
        int number = ep.getEndpointNumber();
        ByteArrayOutputStream batch = new ByteArrayOutputStream();
        boolean[] posted = {false};
        Runnable flush = () -> {
            byte[] bytes;
            synchronized (batch) {
                bytes = batch.toByteArray();
                batch.reset();
                posted[0] = false;
            }
            if (bytes.length > 0) {
                js("__widscan._in(" + JSONObject.quote(id) + "," + number + ",'" + Base64.encodeToString(bytes, Base64.NO_WRAP) + "')");
            }
        };
        UsbRequest req = new UsbRequest();
        ByteBuffer buf = ByteBuffer.allocate(TRANSFER_SIZE);
        try {
            if (!req.initialize(c.connection, ep)) throw new IllegalStateException("UsbRequest.initialize failed");
            while (!c.closed) {
                buf.clear();
                if (!req.queue(buf)) break;
                UsbRequest done = c.connection.requestWait();
                if (done == null || c.closed) break;
                int n = buf.position();
                if (n == 0) continue;
                synchronized (batch) {
                    batch.write(buf.array(), 0, n);
                    if (!posted[0]) {
                        posted[0] = true;
                        main.post(flush);
                    }
                }
            }
        } catch (Exception e) {
            if (!c.closed) Log.w(TAG, "read", e);
        } finally {
            req.close();
        }
        if (!c.closed) main.post(() -> js("__widscan._inEnd(" + JSONObject.quote(id) + "," + number + ")"));
    }

    @JavascriptInterface
    public void usbTransferOut(String id, int number, String b64, int cb) {
        byte[] data = Base64.decode(b64, Base64.DEFAULT);
        io.execute(() -> {
            Conn c = conns.get(id);
            UsbEndpoint ep = c == null ? null : endpoint(c, number, UsbConstants.USB_DIR_OUT);
            if (ep == null) {
                callback(cb, false, JSONObject.quote("endpoint not available"));
                return;
            }
            int done = 0;
            synchronized (c) { // one OUT transfer at a time keeps the bytes in order
                while (done < data.length) {
                    int n = c.connection.bulkTransfer(ep, data, done, Math.min(TRANSFER_SIZE, data.length - done), TIMEOUT_MS);
                    if (n < 0) {
                        done = -1;
                        break;
                    }
                    done += n;
                }
            }
            callback(cb, true, Integer.toString(done));
        });
    }

    @JavascriptInterface
    public void usbControlOut(String id, int requestType, int request, int value, int index, String b64, int cb) {
        byte[] data = Base64.decode(b64, Base64.DEFAULT);
        io.execute(() -> {
            Conn c = conns.get(id);
            if (c == null) {
                callback(cb, false, JSONObject.quote("device not open"));
                return;
            }
            int n = c.connection.controlTransfer(
                    requestType | UsbConstants.USB_DIR_OUT, request, value, index,
                    data.length == 0 ? null : data, data.length, TIMEOUT_MS);
            callback(cb, true, Integer.toString(n));
        });
    }

    // ---- Speech ----

    private void withTts(Runnable r) {
        main.post(() -> {
            if (tts == null) {
                tts = new TextToSpeech(activity, status -> {
                    ttsReady = status == TextToSpeech.SUCCESS;
                    if (ttsReady) for (Runnable q : ttsQueue) q.run();
                    ttsQueue.clear();
                });
            }
            if (ttsReady) r.run();
            else ttsQueue.add(r);
        });
    }

    @JavascriptInterface
    public void speak(String text, double rate, double pitch, double volume, String lang) {
        withTts(() -> {
            if (!lang.isEmpty()) tts.setLanguage(Locale.forLanguageTag(lang));
            tts.setSpeechRate((float) rate);
            tts.setPitch((float) pitch);
            Bundle params = new Bundle();
            params.putFloat(TextToSpeech.Engine.KEY_PARAM_VOLUME, (float) volume);
            tts.speak(text, TextToSpeech.QUEUE_ADD, params, "callout");
        });
    }

    @JavascriptInterface
    public void stopSpeaking() {
        withTts(() -> tts.stop());
    }

    @JavascriptInterface
    public boolean isSpeaking() {
        return tts != null && ttsReady && tts.isSpeaking();
    }

    // ---- Files ----

    /** Saves a download (a session recording) into Downloads. */
    @JavascriptInterface
    public void saveFile(String name, String mime, String b64) {
        byte[] data = Base64.decode(b64, Base64.DEFAULT);
        String safe = name.replaceAll("[\\\\/:*?\"<>|]", "_");
        io.execute(() -> {
            String where;
            try {
                if (Build.VERSION.SDK_INT >= 29) {
                    ContentResolver cr = activity.getContentResolver();
                    ContentValues v = new ContentValues();
                    v.put(MediaStore.MediaColumns.DISPLAY_NAME, safe);
                    v.put(MediaStore.MediaColumns.MIME_TYPE, mime);
                    v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
                    Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                    if (uri == null) throw new IllegalStateException("could not create the file");
                    try (OutputStream out = cr.openOutputStream(uri)) {
                        if (out == null) throw new IllegalStateException("could not open the file");
                        out.write(data);
                    }
                    where = "Downloads/" + safe;
                } else {
                    File dir = activity.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
                    File f = new File(dir, safe);
                    try (OutputStream out = new FileOutputStream(f)) {
                        out.write(data);
                    }
                    where = f.getPath();
                }
                toast("Saved " + where);
            } catch (Exception e) {
                Log.w(TAG, "save", e);
                toast("Could not save " + safe + ": " + e.getMessage());
            }
        });
    }

    private void toast(String msg) {
        main.post(() -> Toast.makeText(activity, msg, Toast.LENGTH_LONG).show());
    }
}

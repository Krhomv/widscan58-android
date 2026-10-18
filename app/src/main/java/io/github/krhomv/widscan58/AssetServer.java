// Serves the bundled web UI (assets/www) to the WebView from an https origin,
// with the bridge script (assets/bridge) loaded ahead of the UI's own code.
// Firmware the APK does not bundle is fetched from the published web UI when
// flashing. GPL-3.0-or-later.
package io.github.krhomv.widscan58;

import android.content.Context;
import android.content.res.AssetManager;
import android.net.Uri;
import android.util.Log;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

final class AssetServer {
    /** Reserved by Android for apps serving their own assets. */
    static final String HOST = "appassets.androidplatform.net";
    static final String ORIGIN = "https://" + HOST;
    static final String BRIDGE_PATH = "/__android/bridge.js";
    /** Where the in-app flasher gets firmware the APK does not bundle. */
    private static final String FIRMWARE_FALLBACK = "https://capnbry.github.io/widscan58";

    private final AssetManager assets;

    AssetServer(Context context) {
        this.assets = context.getAssets();
    }

    WebResourceResponse serve(WebResourceRequest request) {
        Uri uri = request.getUrl();
        if (!HOST.equals(uri.getHost())) return null;
        String path = uri.getPath() == null ? "/" : uri.getPath();
        if (path.endsWith("/")) path += "index.html";
        try {
            if (path.equals(BRIDGE_PATH)) return ok("text/javascript", assets.open("bridge/android-bridge.js"));
            // The UI's service worker caches for offline use; the app is offline already.
            if (path.equals("/sw.js")) return status(404, "Not Found");
            if (path.equals("/index.html")) return ok("text/html", injectBridge(assets.open("www/index.html")));
            try {
                return ok(mime(path), assets.open("www" + path));
            } catch (IOException missing) {
                if (path.startsWith("/firmware/") && "GET".equals(request.getMethod())) return fetchFirmware(path);
                return status(404, "Not Found");
            }
        } catch (IOException e) {
            Log.w("widscan58", "serve " + path, e);
            return status(500, "Internal Error");
        }
    }

    /** A classic script ahead of the UI's module scripts runs before them. */
    private static InputStream injectBridge(InputStream in) throws IOException {
        String html = new String(readAll(in), StandardCharsets.UTF_8);
        String tag = "<script src=\"." + BRIDGE_PATH + "\"></script>";
        int head = html.indexOf("<head>");
        html = head >= 0 ? html.substring(0, head + 6) + tag + html.substring(head + 6) : tag + html;
        return new ByteArrayInputStream(html.getBytes(StandardCharsets.UTF_8));
    }

    private static WebResourceResponse fetchFirmware(String path) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(FIRMWARE_FALLBACK + path).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(30000);
            int code = c.getResponseCode();
            if (code != 200) return status(code, "Upstream " + code);
            return ok(mime(path), new ByteArrayInputStream(readAll(c.getInputStream())));
        } catch (IOException e) {
            Log.w("widscan58", "firmware " + path, e);
            return status(502, "Offline");
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream s = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16384];
            for (int n; (n = s.read(buf)) > 0; ) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    private static WebResourceResponse ok(String mime, InputStream data) {
        return new WebResourceResponse(mime, mime.startsWith("text/") ? "utf-8" : null, 200, "OK", headers(), data);
    }

    private static WebResourceResponse status(int code, String reason) {
        return new WebResourceResponse("text/plain", "utf-8", code, reason, headers(), new ByteArrayInputStream(new byte[0]));
    }

    private static Map<String, String> headers() {
        Map<String, String> h = new HashMap<>();
        h.put("Cache-Control", "no-cache");
        return h;
    }

    private static String mime(String path) {
        String ext = path.substring(path.lastIndexOf('.') + 1).toLowerCase();
        switch (ext) {
            case "html":
                return "text/html";
            case "js":
            case "mjs":
                return "text/javascript";
            case "css":
                return "text/css";
            case "json":
                return "application/json";
            case "webmanifest":
                return "application/manifest+json";
            case "svg":
                return "image/svg+xml";
            case "png":
                return "image/png";
            case "ico":
                return "image/x-icon";
            case "wasm":
                return "application/wasm";
            default:
                return "application/octet-stream";
        }
    }
}

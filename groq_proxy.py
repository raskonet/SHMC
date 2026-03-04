#!/usr/bin/env python3
"""
Groq proxy - listens on port 8002, forwards to Groq API.
Usage: GROQ_API_KEY=your_key_here python3 groq_proxy.py
"""
import http.server, http.client, json, os, sys

GROQ_HOST = "api.groq.com"
GROQ_MODEL = "llama-3.3-70b-versatile"  # free, fast, smart
PORT = 8002
API_KEY = os.environ.get("GROQ_API_KEY", "")

if not API_KEY:
    print("ERROR: set GROQ_API_KEY environment variable")
    print("  export GROQ_API_KEY=gsk_...")
    sys.exit(1)

class ProxyHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[proxy] {fmt % args}")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        data = json.loads(body)

        # Replace whatever model was requested with Groq model
        data["model"] = GROQ_MODEL
        data.pop("temperature", None)  # keep it, groq supports it
        # Groq doesn't want extra fields
        for field in ["cache_prompt", "n_predict"]:
            data.pop(field, None)

        payload = json.dumps(data).encode()

        conn = http.client.HTTPSConnection(GROQ_HOST, timeout=60)
        conn.request("POST", "/openai/v1/chat/completions", body=payload, headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {API_KEY}",
        })
        res = conn.getresponse()
        resp_body = res.read()
        conn.close()

        self.send_response(res.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp_body)))
        self.end_headers()
        self.wfile.write(resp_body)

    def do_GET(self):
        # Health check
        body = b'{"status":"ok","model":"' + GROQ_MODEL.encode() + b'"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

print(f"[proxy] Groq proxy listening on port {PORT}")
print(f"[proxy] Forwarding to Groq model: {GROQ_MODEL}")
print(f"[proxy] compose.py needs no changes")
httpd = http.server.HTTPServer(("127.0.0.1", PORT), ProxyHandler)
httpd.serve_forever()

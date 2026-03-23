#!/usr/bin/env python3
"""Simple registration form server."""

from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, unquote_plus

PORT = 8080

FORM_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Create Account</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #0f0f13;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    color: #e2e2e8;
  }

  .card {
    width: 100%;
    max-width: 420px;
    background: #18181f;
    border: 1px solid #2a2a36;
    border-radius: 16px;
    padding: 40px 36px 36px;
    box-shadow: 0 24px 64px rgba(0,0,0,.5);
  }

  .logo {
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: linear-gradient(135deg, #7c6af5, #a78bfa);
    display: flex;
    align-items: center;
    justify-content: center;
    margin-bottom: 24px;
    font-size: 20px;
  }

  h1 {
    font-size: 1.4rem;
    font-weight: 600;
    color: #f0f0f5;
    margin-bottom: 6px;
  }

  .subtitle {
    font-size: .875rem;
    color: #6b6b80;
    margin-bottom: 28px;
  }

  .row {
    display: flex;
    gap: 12px;
  }

  .field {
    display: flex;
    flex-direction: column;
    margin-bottom: 16px;
    flex: 1;
  }

  label {
    font-size: .75rem;
    font-weight: 500;
    color: #9090a8;
    letter-spacing: .04em;
    text-transform: uppercase;
    margin-bottom: 6px;
  }

  input {
    background: #0f0f13;
    border: 1px solid #2a2a36;
    border-radius: 8px;
    padding: 10px 14px;
    font-size: .9rem;
    color: #e2e2e8;
    outline: none;
    transition: border-color .15s, box-shadow .15s;
    width: 100%;
  }

  input:focus {
    border-color: #7c6af5;
    box-shadow: 0 0 0 3px rgba(124,106,245,.15);
  }

  input::placeholder { color: #3a3a50; }

  button {
    width: 100%;
    margin-top: 8px;
    padding: 12px;
    border: none;
    border-radius: 8px;
    background: linear-gradient(135deg, #7c6af5, #a78bfa);
    color: #fff;
    font-size: .95rem;
    font-weight: 600;
    cursor: pointer;
    transition: opacity .15s, transform .1s;
    letter-spacing: .02em;
  }

  button:hover { opacity: .9; }
  button:active { transform: scale(.98); }

  .footer {
    margin-top: 20px;
    font-size: .8rem;
    color: #3a3a50;
    text-align: center;
  }
</style>
</head>
<body>
<div class="card">
  <div class="logo">✦</div>
  <h1>Create an account</h1>
  <p class="subtitle">Fill in your details to get started.</p>
  <form method="POST" action="/">
    <div class="row">
      <div class="field">
        <label for="first">First name</label>
        <input id="first" name="first" type="text" placeholder="Ada" autocomplete="given-name">
      </div>
      <div class="field">
        <label for="last">Last name</label>
        <input id="last" name="last" type="text" placeholder="Lovelace" autocomplete="family-name">
      </div>
    </div>
    <div class="field">
      <label for="username">Username</label>
      <input id="username" name="username" type="text" placeholder="ada_lovelace" autocomplete="username">
    </div>
    <div class="field">
      <label for="email">Email</label>
      <input id="email" name="email" type="text" placeholder="ada@example.com" autocomplete="email">
    </div>
    <div class="field">
      <label for="password">Password</label>
      <input id="password" name="password" type="password" placeholder="••••••••" autocomplete="new-password">
    </div>
    <button type="submit">Create account →</button>
  </form>
  <p class="footer">No validation. No judgment.</p>
</div>
</body>
</html>
"""

THANKS_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>You're in</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    background: #0f0f13;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    color: #e2e2e8;
  }

  .card {
    width: 100%;
    max-width: 420px;
    background: #18181f;
    border: 1px solid #2a2a36;
    border-radius: 16px;
    padding: 48px 36px;
    box-shadow: 0 24px 64px rgba(0,0,0,.5);
    text-align: center;
  }

  .check {
    width: 56px;
    height: 56px;
    border-radius: 50%;
    background: linear-gradient(135deg, #7c6af5, #a78bfa);
    display: flex;
    align-items: center;
    justify-content: center;
    margin: 0 auto 24px;
    font-size: 24px;
  }

  h1 {
    font-size: 1.5rem;
    font-weight: 600;
    color: #f0f0f5;
    margin-bottom: 10px;
  }

  p {
    font-size: .9rem;
    color: #6b6b80;
    line-height: 1.6;
    margin-bottom: 28px;
  }

  .name {
    color: #a78bfa;
    font-weight: 500;
  }

  a {
    display: inline-block;
    padding: 10px 24px;
    border-radius: 8px;
    background: #1e1e2a;
    border: 1px solid #2a2a36;
    color: #9090a8;
    font-size: .85rem;
    text-decoration: none;
    transition: border-color .15s, color .15s;
  }

  a:hover { border-color: #7c6af5; color: #a78bfa; }
</style>
</head>
<body>
<div class="card">
  <div class="check">✓</div>
  <h1>Welcome{name_part}!</h1>
  <p>Your account has been created.<br>You're all set to get started.</p>
  <a href="/">← Back to form</a>
</div>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"  {self.address_string()} {fmt % args}")

    def do_GET(self):
        self._send(200, FORM_HTML)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        data = {k: unquote_plus(v[0]) if v else "" for k, v in parse_qs(body).items()}
        first = data.get("first", "").strip()
        name_part = f", {first}" if first else ""
        self._send(200, THANKS_HTML.replace("{name_part}", name_part))

    def _send(self, code, html):
        body = html.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    server = HTTPServer(("localhost", PORT), Handler)
    print(f"Serving at http://localhost:{PORT}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
藤のnetdisk Sidecar —— 给 C++ 网盘补齐「外网 HTTPS」能力。

职责：
  1. AI 网关：持有 API Key，向任意 OpenAI 兼容接口（OpenAI / DeepSeek /
     Moonshot / 通义等）转发聊天请求，C++ 服务器只在本机访问本服务；
  2. OAuth：GitHub、Apple 登录的授权码交换与身份获取。

浏览器永远不直接访问本服务，所以 API Key / OAuth Secret 不会暴露到前端。

用法：
  python3 sidecar/bridge.py --config ./sidecar-config.json

curl 自测（C++ 服务端也是按同样路径调用）：
  curl http://127.0.0.1:18666/health
"""

import argparse
import base64
import json
import os
import re
import secrets
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


DEFAULTS = {
    "listen": "127.0.0.1:18666",
    "publicBaseUrl": "http://127.0.0.1:10000",
    "ai": {
        "baseUrl": "https://api.openai.com/v1",
        "apiKey": "",
        "model": "gpt-4o-mini",
    },
    "github": {"clientId": "", "clientSecret": ""},
    "apple": {
        "serviceId": "",
        "teamId": "",
        "keyId": "",
        "privateKeyPath": "",
    },
}

CONFIG_PATH = "sidecar-config.json"
CONFIG = {}
CONFIG_LOCK = threading.Lock()
PENDING_STATE = {}
PENDING_LOCK = threading.Lock()


def deep_merge(base, extra):
    out = dict(base)
    for key, value in (extra or {}).items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def env_config():
    cfg = {}
    mapping = {
        "SIDECAR_LISTEN": ("listen", str),
        "SIDECAR_PUBLIC_BASE_URL": ("publicBaseUrl", str),
        "SIDECAR_AI_BASE_URL": ("ai", "baseUrl"),
        "SIDECAR_AI_API_KEY": ("ai", "apiKey"),
        "SIDECAR_AI_MODEL": ("ai", "model"),
        "SIDECAR_GITHUB_CLIENT_ID": ("github", "clientId"),
        "SIDECAR_GITHUB_CLIENT_SECRET": ("github", "clientSecret"),
        "SIDECAR_APPLE_SERVICE_ID": ("apple", "serviceId"),
        "SIDECAR_APPLE_TEAM_ID": ("apple", "teamId"),
        "SIDECAR_APPLE_KEY_ID": ("apple", "keyId"),
        "SIDECAR_APPLE_PRIVATE_KEY_PATH": ("apple", "privateKeyPath"),
    }
    for name, (section, key) in mapping.items():
        value = os.environ.get(name)
        if value:
            if key is str:
                # ("listen", str) 表示该环境变量是顶层标量
                cfg[section] = value
            else:
                # ("ai", "apiKey") 表示嵌套小节字段
                cfg.setdefault(section, {})[key] = value
    return cfg


def load_config():
    global CONFIG
    cfg = deep_merge(DEFAULTS, {})
    try:
        if Path(CONFIG_PATH).is_file():
            with open(CONFIG_PATH, "r", encoding="utf-8") as handle:
                cfg = deep_merge(cfg, json.load(handle))
    except Exception as exc:  # 配置文件坏了也不阻塞服务，错误会在 /ai/status 显示
        cfg["_loadError"] = str(exc)
    cfg = deep_merge(cfg, env_config())
    with CONFIG_LOCK:
        CONFIG = cfg
    return cfg


def save_config(cfg):
    with CONFIG_LOCK:
        CONFIG = cfg
    try:
        path = Path(CONFIG_PATH)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = path.with_name(path.name + ".tmp")
        with open(tmp_path, "w", encoding="utf-8") as handle:
            json.dump(cfg, handle, ensure_ascii=False, indent=2)
        os.chmod(tmp_path, 0o600)
        os.replace(tmp_path, path)
        return True, ""
    except OSError as exc:
        return False, "无法写入配置文件：" + str(exc)


def http_request(url, data=None, headers=None, form=False, timeout=180):
    request_headers = dict(headers or {})
    body = None
    if data is not None:
        if form:
            body = urllib.parse.urlencode(data).encode("utf-8")
            request_headers.setdefault("Content-Type",
                "application/x-www-form-urlencoded")
        else:
            body = json.dumps(data, ensure_ascii=False).encode("utf-8")
            request_headers.setdefault("Content-Type", "application/json")
    request = urllib.request.Request(url, data=body, headers=request_headers)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        try:
            parsed = json.loads(detail)
            detail = parsed.get("error", parsed.get("message", detail))
        except Exception:
            pass
        raise RuntimeError("HTTP %s：%s" % (exc.code, detail[:600])) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError("网络错误：%s" % (exc.reason or exc)) from exc
    try:
        return json.loads(raw.decode("utf-8") or "null")
    except Exception:
        return raw.decode("utf-8", "replace")


def api_configured():
    return bool((CONFIG.get("ai") or {}).get("apiKey"))


def handle_ai_status():
    ai = CONFIG.get("ai") or {}
    return {
        "ok": True,
        "configured": bool(ai.get("apiKey")),
        "baseUrl": ai.get("baseUrl", ""),
        "model": ai.get("model", ""),
        "apiKeyMasked": ("***" if ai.get("apiKey") else ""),
    }


def handle_oauth_status():
    github = CONFIG.get("github") or {}
    apple = CONFIG.get("apple") or {}
    return {
        "ok": True,
        "github": bool(github.get("clientId") and github.get("clientSecret")),
        "apple": bool(apple.get("serviceId") and apple.get("teamId") and
                      apple.get("keyId") and apple.get("privateKeyPath")),
    }


def sanitize_identity(value, fallback="user"):
    cleaned = re.sub(r"[^A-Za-z0-9_.-]", "_", value or "").strip("_")
    cleaned = re.sub(r"_{2,}", "_", cleaned)
    return cleaned[:48] or fallback


def new_state(provider):
    state = secrets.token_urlsafe(24)
    now = time.time()
    with PENDING_LOCK:
        PENDING_STATE[state] = {"provider": provider, "expires": now + 600}
        for key, item in list(PENDING_STATE.items()):
            if item["expires"] <= now:
                del PENDING_STATE[key]
    return state


def consume_state(state, provider):
    if not state:
        return False
    with PENDING_LOCK:
        item = PENDING_STATE.pop(state, None)
    return bool(item and item.get("provider") == provider)


def redirect_uri(provider):
    base = str(CONFIG.get("publicBaseUrl") or "http://127.0.0.1:10000").rstrip("/")
    return base + "/api/oauth/" + provider + "/callback"


def handle_github_begin():
    github = CONFIG.get("github") or {}
    if not github.get("clientId"):
        return 503, {"ok": False, "error": "GitHub OAuth 未配置（sidecar-config.json 缺少 clientId）"}
    params = {
        "client_id": github["clientId"],
        "redirect_uri": redirect_uri("github"),
        "scope": "read:user user:email",
        "state": new_state("github"),
    }
    url = "https://github.com/login/oauth/authorize?" + urllib.parse.urlencode(params)
    return 200, {"ok": True, "authorizeUrl": url}


def handle_github_callback(query):
    params = urllib.parse.parse_qs(query, keep_blank_values=True)
    code = (params.get("code") or [""])[0]
    state = (params.get("state") or [""])[0]
    if not code:
        return 400, {"ok": False, "error": "GitHub 未返回授权码"}
    if not consume_state(state, "github"):
        return 400, {"ok": False, "error": "state 无效或已过期，请重新登录"}
    github = CONFIG.get("github") or {}
    token_data = http_request(
        "https://github.com/login/oauth/access_token",
        {
            "client_id": github.get("clientId", ""),
            "client_secret": github.get("clientSecret", ""),
            "code": code,
            "redirect_uri": redirect_uri("github"),
        },
        headers={"Accept": "application/json"},
        form=True,
        timeout=30,
    )
    access_token = token_data.get("access_token")
    if not access_token:
        return 502, {"ok": False, "error": "GitHub 换取 token 失败：" + str(token_data)[:300]}
    user = http_request(
        "https://api.github.com/user",
        headers={
            "Authorization": "Bearer " + access_token,
            "Accept": "application/vnd.github+json",
            "User-Agent": "FujiNetdisk",
        },
        timeout=30,
    )
    if not isinstance(user, dict) or not user.get("id"):
        return 502, {"ok": False, "error": "GitHub 用户信息获取失败"}
    return 200, {
        "ok": True,
        "provider": "github",
        "username": "gh-" + sanitize_identity(user.get("login"), "user"),
        "origin": "github:" + str(user.get("id")),
        "displayName": str(user.get("login", "")),
        "email": user.get("email") or "",
    }


def handle_apple_begin():
    apple = CONFIG.get("apple") or {}
    if not apple.get("serviceId"):
        return 503, {"ok": False, "error": "Apple 登录未配置（缺少 serviceId）"}
    params = {
        "client_id": apple["serviceId"],
        "redirect_uri": redirect_uri("apple"),
        "response_type": "code",
        "response_mode": "form_post",
        "scope": "name email",
        "state": new_state("apple"),
    }
    url = "https://appleid.apple.com/auth/authorize?" + urllib.parse.urlencode(params)
    return 200, {"ok": True, "authorizeUrl": url}


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def apple_client_secret():
    """用 p8 私钥生成 Apple 要求的 ES256 JWT client_secret。"""
    apple = CONFIG.get("apple") or {}
    required = ["serviceId", "teamId", "keyId"]
    for key in required:
        if not apple.get(key):
            raise RuntimeError("Apple 配置缺少 " + key)
    key_path = apple.get("privateKeyPath") or ""
    if not key_path:
        raise RuntimeError("Apple 配置缺少 privateKeyPath（p8 私钥文件）")
    candidate = Path(key_path)
    if not candidate.is_absolute():
        candidate = Path(CONFIG_PATH).parent / candidate
    if not candidate.is_file():
        raise RuntimeError("Apple p8 私钥不存在：" + key_path)
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        raise RuntimeError("Apple 登录需要 cryptography：pip install -r sidecar/requirements.txt")

    private_key = serialization.load_pem_private_key(candidate.read_bytes(), password=None)
    header = {"alg": "ES256", "kid": apple["keyId"]}
    now = int(time.time())
    claims = {
        "iss": apple["teamId"],
        "iat": now,
        "exp": now + 3600,
        "aud": "https://appleid.apple.com",
        "sub": apple["serviceId"],
    }
    signing_input = (
        b64url(json.dumps(header, separators=(",", ":")).encode("ascii")) + "." +
        b64url(json.dumps(claims, separators=(",", ":")).encode("ascii"))
    )
    der_signature = private_key.sign(
        signing_input.encode("ascii"), ec.ECDSA(hashes.SHA256()))
    r, s = utils.decode_dss_signature(der_signature)
    raw = r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return signing_input + "." + b64url(raw)


def b64url_decode(text):
    text = str(text)
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def jwk_integer(value):
    return int.from_bytes(b64url_decode(value), "big")


def verify_apple_id_token(token, expected_aud):
    """校验 Apple 返回的 id_token：RS256 签名 + iss/aud/sub/exp 声明。"""
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import padding, rsa
    except ImportError:
        raise RuntimeError(
            "Apple 登录需要 cryptography：pip install -r sidecar/requirements.txt")
    try:
        signing_input, signature_part = token.rsplit(".", 1)
        header_part, payload_part = signing_input.split(".", 1)
        header = json.loads(b64url_decode(header_part).decode("utf-8"))
        signature = b64url_decode(signature_part)
    except Exception as exc:
        raise RuntimeError("Apple id_token 格式无效") from exc
    if header.get("alg") != "RS256":
        raise RuntimeError("Apple id_token 算法不受支持：" + str(header.get("alg")))

    keys = http_request("https://appleid.apple.com/auth/keys", timeout=30)
    jwk = next((item for item in keys.get("keys", [])
                if item.get("kid") == header.get("kid")), None)
    if not jwk:
        raise RuntimeError("找不到匹配的 Apple 公钥（kid）")
    try:
        public_key = rsa.RSAPublicNumbers(
            jwk_integer(jwk["e"]), jwk_integer(jwk["n"])).public_key()
        public_key.verify(signature, signing_input.encode("utf-8"),
                          padding.PKCS1v15(), hashes.SHA256())
    except (KeyError, TypeError, ValueError, InvalidSignature) as exc:
        raise RuntimeError("Apple id_token 签名校验失败") from exc

    try:
        claims = json.loads(b64url_decode(payload_part).decode("utf-8"))
    except Exception as exc:
        raise RuntimeError("Apple id_token 载荷解析失败") from exc
    now = int(time.time())
    if claims.get("iss") != "https://appleid.apple.com":
        raise RuntimeError("Apple id_token issuer 不匹配")
    if claims.get("aud") != expected_aud:
        raise RuntimeError("Apple id_token 受众不匹配")
    if not claims.get("sub"):
        raise RuntimeError("Apple id_token 缺少 sub")
    exp = claims.get("exp")
    if not isinstance(exp, int) or exp < now - 60:
        raise RuntimeError("Apple id_token 已过期")
    return claims


def handle_apple_callback(form_text):
    fields = urllib.parse.parse_qs(form_text, keep_blank_values=True)
    code = (fields.get("code") or [""])[0]
    state = (fields.get("state") or [""])[0]
    if not code:
        return 400, {"ok": False, "error": "Apple 未返回授权码"}
    if not consume_state(state, "apple"):
        return 400, {"ok": False, "error": "state 无效或已过期，请重新登录"}

    try:
        secret = apple_client_secret()
    except RuntimeError as exc:
        return 503, {"ok": False, "error": str(exc)}

    apple = CONFIG.get("apple") or {}
    try:
        token_data = http_request(
            "https://appleid.apple.com/auth/token",
            {
                "client_id": apple.get("serviceId", ""),
                "client_secret": secret,
                "code": code,
                "grant_type": "authorization_code",
                "redirect_uri": redirect_uri("apple"),
            },
            form=True,
            timeout=30,
        )
    except RuntimeError as exc:
        return 502, {"ok": False, "error": str(exc)}
    id_token = token_data.get("id_token") if isinstance(token_data, dict) else ""
    if not id_token:
        return 502, {"ok": False, "error": "Apple 未返回 id_token"}

    try:
        claims = verify_apple_id_token(id_token, apple.get("serviceId", ""))
    except RuntimeError as exc:
        return 502, {"ok": False, "error": str(exc)}
    subject = str(claims.get("sub") or "")
    email = str(claims.get("email") or "")
    identity = email.split("@")[0] if email else subject[:16]
    return 200, {
        "ok": True,
        "provider": "apple",
        "username": "apple-" + sanitize_identity(identity, "user"),
        "origin": "apple:" + subject,
        "displayName": str(claims.get("email") or subject[:8]),
        "email": email,
    }


def handle_ai_chat(payload):
    ai = CONFIG.get("ai") or {}
    if not ai.get("apiKey"):
        return 503, {"ok": False, "error": "AI 未配置：请在「AI 设置」填写 API Key"}
    if not payload.get("messages"):
        return 400, {"ok": False, "error": "messages 不能为空"}
    request_body = {
        "model": payload.get("model") or ai.get("model") or "gpt-4o-mini",
        "messages": payload["messages"],
        "temperature": float(payload.get("temperature", 0.7)),
        "stream": False,
    }
    max_tokens = payload.get("maxTokens")
    if max_tokens:
        request_body["max_tokens"] = int(max_tokens)
    base = str(ai.get("baseUrl") or "https://api.openai.com/v1").rstrip("/")
    try:
        data = http_request(
            base + "/chat/completions",
            request_body,
            headers={"Authorization": "Bearer " + ai["apiKey"]},
            timeout=240,
        )
    except RuntimeError as exc:
        return 502, {"ok": False, "error": str(exc)}
    try:
        content = data["choices"][0]["message"]["content"]
        return 200, {"ok": True, "model": data.get("model", ""), "content": content or ""}
    except Exception:
        return 502, {"ok": False, "error": "AI 返回格式异常：" + str(data)[:500]}


def handle_ai_config(payload):
    with CONFIG_LOCK:
        current = CONFIG
    ai = dict(current.get("ai") or {})
    if "apiKey" in payload:
        ai["apiKey"] = str(payload.get("apiKey") or "")
    if "baseUrl" in payload:
        base = str(payload.get("baseUrl") or "").strip().rstrip("/")
        if base:
            ai["baseUrl"] = base
    if "model" in payload:
        model = str(payload.get("model") or "").strip()
        if model:
            ai["model"] = model
    updated = deep_merge(current, {"ai": ai})
    ok, error = save_config(updated)
    if not ok:
        return 500, {"ok": False, "error": error}
    return 200, {"ok": True, "configured": bool(ai.get("apiKey")), "model": ai.get("model")}


class SidecarHandler(BaseHTTPRequestHandler):
    server_version = "FujiSidecar/1.0"

    def log_message(self, fmt, *args):
        pass    # C++ 服务器已有访问日志，这里保持安静

    def _json(self, code, payload):
        raw = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 8 * 1024 * 1024:
            raise RuntimeError("body too large")
        return self.rfile.read(length).decode("utf-8", "replace")

    def _route_get(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        if path == "/health":
            return self._json(200, {"ok": True, "service": "fuji-sidecar"})
        if path == "/ai/status":
            return self._json(200, handle_ai_status())
        if path == "/oauth/status":
            return self._json(200, handle_oauth_status())
        if path == "/oauth/github/begin":
            code, payload = handle_github_begin()
            return self._json(code, payload)
        if path == "/oauth/github/callback":
            try:
                code, payload = handle_github_callback(parsed.query)
            except RuntimeError as exc:
                code, payload = 502, {"ok": False, "error": str(exc)}
            return self._json(code, payload)
        if path == "/oauth/apple/begin":
            code, payload = handle_apple_begin()
            return self._json(code, payload)
        return self._json(404, {"ok": False, "error": "not found"})

    def _route_post(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        try:
            body_text = self._read_body()
        except RuntimeError as exc:
            return self._json(413, {"ok": False, "error": str(exc)})
        if path == "/ai/chat":
            try:
                payload = json.loads(body_text or "{}")
            except Exception:
                return self._json(400, {"ok": False, "error": "JSON 解析失败"})
            code, result = handle_ai_chat(payload)
            return self._json(code, result)
        if path == "/ai/config":
            try:
                payload = json.loads(body_text or "{}")
            except Exception:
                return self._json(400, {"ok": False, "error": "JSON 解析失败"})
            code, result = handle_ai_config(payload)
            return self._json(code, result)
        if path == "/oauth/apple/callback":
            code, result = handle_apple_callback(body_text)
            return self._json(code, result)
        return self._json(404, {"ok": False, "error": "not found"})

    def do_GET(self):
        try:
            load_config()
            self._route_get()
        except BrokenPipeError:
            pass
        except Exception as exc:
            try:
                self._json(500, {"ok": False, "error": "sidecar internal error: " + str(exc)})
            except Exception:
                pass

    def do_POST(self):
        try:
            load_config()
            self._route_post()
        except BrokenPipeError:
            pass
        except Exception as exc:
            try:
                self._json(500, {"ok": False, "error": "sidecar internal error: " + str(exc)})
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description="藤のnetdisk AI/OAuth sidecar")
    parser.add_argument("--config", default="sidecar-config.json",
        help="配置文件路径（JSON），默认 ./sidecar-config.json")
    parser.add_argument("--host", default=None, help="监听地址（默认读配置）")
    parser.add_argument("--port", type=int, default=None, help="监听端口（默认读配置）")
    args = parser.parse_args()

    global CONFIG_PATH
    CONFIG_PATH = args.config
    load_config()
    host, port = str(CONFIG.get("listen") or "127.0.0.1:18666").rsplit(":", 1)
    if args.host:
        host = args.host
    if args.port:
        port = args.port
    server = ThreadingHTTPServer((host, int(port)), SidecarHandler)
    print("FujiNetdisk sidecar listening on %s:%s (config: %s)" % (host, port, CONFIG_PATH),
        flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

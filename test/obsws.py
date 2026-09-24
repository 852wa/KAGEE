"""Minimal obs-websocket v5 client for driving visual tests."""
import asyncio
import base64
import json
import uuid

import websockets


class OBS:
    def __init__(self, url="ws://127.0.0.1:4466"):
        self.url = url
        self.ws = None

    async def __aenter__(self):
        for _ in range(60):
            try:
                self.ws = await websockets.connect(self.url, max_size=64 * 1024 * 1024)
                break
            except OSError:
                await asyncio.sleep(1)
        else:
            raise RuntimeError("cannot connect to obs-websocket")
        await self.ws.recv()  # Hello
        await self.ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1, "eventSubscriptions": 0}}))
        await self.ws.recv()  # Identified
        return self

    async def __aexit__(self, *exc):
        await self.ws.close()

    async def call(self, request_type, data=None):
        rid = str(uuid.uuid4())
        await self.ws.send(json.dumps({"op": 6, "d": {
            "requestType": request_type, "requestId": rid, "requestData": data or {}}}))
        while True:
            msg = json.loads(await self.ws.recv())
            if msg["op"] == 7 and msg["d"]["requestId"] == rid:
                st = msg["d"]["requestStatus"]
                if not st["result"]:
                    raise RuntimeError(f"{request_type} failed: {st}")
                return msg["d"].get("responseData", {})

    async def screenshot(self, source, path, width=960):
        r = await self.call("GetSourceScreenshot", {
            "sourceName": source, "imageFormat": "png", "imageWidth": width})
        b64 = r["imageData"].split(",", 1)[1]
        with open(path, "wb") as f:
            f.write(base64.b64decode(b64))

    async def settings(self, name, settings):
        await self.call("SetInputSettings", {"inputName": name, "inputSettings": settings, "overlay": True})

    async def hotkey(self, name, context):
        await self.call("TriggerHotkeyByName", {"hotkeyName": name, "contextName": context})

#!/usr/bin/env python3
"""Exercise discovery, two streams, removal, respawn and RGB format rejection.

Requires a working renderer and the selected distro's `gz` CLI in the environment.
Control messages use Gazebo services; camera bytes still use Camera::Copy().
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("distro", choices=("harmonic", "jetty"))
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("--headless-rendering", action="store_true",
                        help="Use EGL instead of an available X11 display")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    world_file = root / "examples" / args.distro / "world.sdf"
    world = ET.parse(world_file).getroot().find("world")
    model = next(m for m in world.findall("model") if m.findall(".//sensor[@type='camera']"))
    cameras = model.findall(".//sensor[@type='camera']")
    assert len(cameras) >= 2, "integration world must contain at least two cameras"
    camera_names = [s.attrib["name"] for s in cameras]
    model_name, world_name = model.attrib["name"], world.attrib["name"]
    model_sdf = '<sdf version="1.9">' + ET.tostring(model, encoding="unicode") + "</sdf>"
    log_file = build / "dynamic-cameras.log"
    env = os.environ.copy()
    env["GZ_SIM_SYSTEM_PLUGIN_PATH"] = str(build) + os.pathsep + env.get("GZ_SIM_SYSTEM_PLUGIN_PATH", "")
    env["GZ_PARTITION"] = "gst-camera-dynamic-" + uuid.uuid4().hex
    env["GZ_IP"] = "127.0.0.1"
    with tempfile.TemporaryDirectory(prefix="gst-camera-dynamic-") as temp:
        env["GZ_HOMEDIR"] = temp
        with log_file.open("w") as output:
            major = "8" if args.distro == "harmonic" else "10"
            command = ["gz", "sim", "--force-version", major,
                       "-s", "-r", "-v", "4", str(world_file)]
            if args.headless_rendering or not env.get("DISPLAY"):
                command.append("--headless-rendering")
            server = subprocess.Popen(command,
                                      env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                def wait_for(predicate, description, timeout=45):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        logs = log_file.read_text(errors="replace")
                        if predicate(logs):
                            return
                        if server.poll() is not None:
                            raise RuntimeError(f"server exited ({server.returncode}) waiting for {description}; see {log_file}")
                        time.sleep(0.1)
                    raise RuntimeError(f"timed out waiting for {description}; see {log_file}")

                def frames(logs, count):
                    return all(logs.count(f"APP_SRC_FIRST_FRAME [{name}]") >= count for name in camera_names)

                def service(name, request_type, request):
                    result = subprocess.run(["gz", "service", "-s", f"/world/{world_name}/{name}",
                                             "--reqtype", request_type, "--reptype", "gz.msgs.Boolean",
                                             "--timeout", "10000", "--req", request],
                                            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            timeout=15, check=True)
                    if "data: true" not in result.stdout:
                        raise RuntimeError(f"{name} request failed: {result.stdout}")

                wait_for(lambda logs: frames(logs, 1), "both initial appsrc streams")
                for sensor in cameras:
                    config = sensor.find("plugin")
                    destination = config.findtext("udp_host", "127.0.0.1") + ":" + config.findtext("udp_port", "5600")
                    logs = log_file.read_text(errors="replace")
                    expected = f"FPS -> {destination} using"
                    assert expected in logs, f"missing independent destination {destination}"
                service("remove", "gz.msgs.Entity", f"name: {json.dumps(model_name)} type: MODEL")
                wait_for(lambda logs: logs.count("removed camera entity") >= len(cameras), "all camera streams removed")
                service("create_multiple", "gz.msgs.EntityFactory_V",
                        f"data: {{ sdf: {json.dumps(model_sdf)} allow_renaming: false }}")
                wait_for(lambda logs: frames(logs, 2), "both respawned appsrc streams")

                # A valid monochrome rendering camera must never be treated as
                # RGB, and rejecting it must not stop the other camera's stream.
                service("remove", "gz.msgs.Entity", f"name: {json.dumps(model_name)} type: MODEL")
                wait_for(lambda logs: logs.count("removed camera entity") >= 2 * len(cameras),
                         "second camera removal")
                cameras[0].find("camera/image/format").text = "L8"
                monochrome_sdf = '<sdf version="1.9">' + ET.tostring(model, encoding="unicode") + "</sdf>"
                service("create_multiple", "gz.msgs.EntityFactory_V",
                        f"data: {{ sdf: {json.dumps(monochrome_sdf)} allow_renaming: false }}")
                wait_for(lambda logs: "requires R8G8B8; unsupported rendering format" in logs
                         and logs.count(f"APP_SRC_FIRST_FRAME [{camera_names[1]}]") >= 3,
                         "monochrome rejection with the other RGB stream running")
                assert log_file.read_text().count(f"APP_SRC_FIRST_FRAME [{camera_names[0]}]") == 2
            finally:
                if server.poll() is None:
                    os.killpg(server.pid, signal.SIGINT)
                    try:
                        server.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(server.pid, signal.SIGKILL)
                        server.wait()
                        raise RuntimeError(f"server failed deterministic shutdown; see {log_file}")
        if server.returncode != 0:
            raise RuntimeError(f"server shutdown returned {server.returncode}; see {log_file}")
    logs = log_file.read_text(errors="replace")
    for failure in ("SystemLoader.cc", "Failed to load system plugin", "Segmentation fault"):
        assert failure not in logs, f"unexpected {failure}; see {log_file}"
    print(f"PASS {args.distro}: two destinations, removal, respawn, RGB format rejection and shutdown; {log_file}")


if __name__ == "__main__":
    main()

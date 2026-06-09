import argparse
import struct
import sys

import numpy as np
from ultralytics import YOLO


def read_exact(stream, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--conf", type=float, default=0.25)
    args = parser.parse_args()

    model = YOLO(args.model)
    stdin = sys.stdin.buffer
    stdout = sys.stdout

    print(f"YOLO worker ready: {args.model}, input {args.imgsz}", file=sys.stderr, flush=True)

    while True:
        header = read_exact(stdin, 12)
        if header is None:
            break

        width, height, payload_size = struct.unpack("<III", header)
        payload = read_exact(stdin, payload_size)
        if payload is None:
            break

        rgb_image = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, 3))
        image = rgb_image[:, :, ::-1].copy()
        result = model.predict(image, imgsz=args.imgsz, conf=args.conf, verbose=False)[0]

        obb = getattr(result, "obb", None)
        if obb is None or len(obb) == 0:
            stdout.write("0\n")
            stdout.flush()
            continue

        corners = obb.xyxyxyxy.cpu().numpy()
        confidences = obb.conf.cpu().numpy()
        classes = obb.cls.cpu().numpy().astype(int)
        count = len(corners)

        stdout.write(f"{count}\n")
        for class_id, confidence, box in zip(classes, confidences, corners):
            flat = box.reshape(-1)
            stdout.write(
                f"{class_id} {confidence:.6f} "
                f"{flat[0]:.3f} {flat[1]:.3f} "
                f"{flat[2]:.3f} {flat[3]:.3f} "
                f"{flat[4]:.3f} {flat[5]:.3f} "
                f"{flat[6]:.3f} {flat[7]:.3f}\n"
            )
        stdout.flush()


if __name__ == "__main__":
    main()

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3-pymongo python3-pymysql && rm -rf /var/lib/apt/lists/*
COPY smoke.py /smoke.py
ENTRYPOINT ["python3", "/smoke.py"]

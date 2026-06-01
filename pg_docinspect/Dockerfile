# Build pg_docinspect on top of the documentdb-local image
#
# Build (from the pg_docinspect directory):
#   docker build -t pg-docinspect .
#
# Run as a background container:
#   docker run -d --name lab pg-docinspect --password mypass
#
# Connect via psql:
#   docker exec -it lab psql -h localhost -p 9712 -U documentdb -d postgres
#
# Connect via mongosh:
#   docker exec -it lab mongosh --port 10260 -u default_user -p mypass
#
# Then:
#   CREATE EXTENSION docinspect;
#   SELECT docinspect.bson_inspect_pretty('{"hello": "world"}'::documentdb_core.bson);

# ----------- Builder stage -----------
FROM ghcr.io/documentdb/documentdb/documentdb-local:latest AS builder

USER root

# Detect the installed PG version
RUN PG_VER=$(pg_config --version | sed 's/PostgreSQL //' | cut -d. -f1) && \
    echo "$PG_VER" > /tmp/pg_version

# Install build dependencies (PG dev headers + libbson)
RUN PG_VER=$(cat /tmp/pg_version) && \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    pkg-config \
    postgresql-server-dev-${PG_VER} \
    libmongoc-dev \
    libbson-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy extension source
COPY . /tmp/pg_docinspect
WORKDIR /tmp/pg_docinspect

# Build and install the extension
RUN make && make install

# ----------- Final image: clean runtime with extension installed -----------
FROM ghcr.io/documentdb/documentdb/documentdb-local:latest

USER root

# Copy built extension artifacts from builder (use pg_config to find paths)
COPY --from=builder /tmp/pg_version /tmp/pg_version
RUN PG_VER=$(cat /tmp/pg_version) && \
    echo "PG version detected: $PG_VER" && \
    rm /tmp/pg_version

COPY --from=builder /tmp/pg_docinspect/pg_docinspect.so /tmp/pg_docinspect.so
COPY --from=builder /tmp/pg_docinspect/sql/docinspect--0.1-0.sql /tmp/docinspect--0.1-0.sql
COPY --from=builder /tmp/pg_docinspect/docinspect.control /tmp/docinspect.control

# Install into the correct PG paths
RUN PG_LIBDIR=$(pg_config --pkglibdir) && \
    PG_SHAREDIR=$(pg_config --sharedir) && \
    cp /tmp/pg_docinspect.so "$PG_LIBDIR/pg_docinspect.so" && \
    cp /tmp/docinspect.control "$PG_SHAREDIR/extension/docinspect.control" && \
    cp /tmp/docinspect--0.1-0.sql "$PG_SHAREDIR/extension/docinspect--0.1-0.sql" && \
    rm -f /tmp/pg_docinspect.so /tmp/docinspect.control /tmp/docinspect--0.1-0.sql

USER documentdb

WORKDIR /home/documentdb/gateway/scripts
ENTRYPOINT ["/bin/bash", "-c", "/home/documentdb/gateway/scripts/emulator_entrypoint.sh \"$@\"", "--"]

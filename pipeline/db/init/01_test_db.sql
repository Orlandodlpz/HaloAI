-- Runs once, when the Docker volume is first created.
-- Tests use their own database so they never touch development data.
CREATE DATABASE haloai_test OWNER haloai;

# Testing this extension

The `sql` directory holds [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). Run them with

```bash
make test_release
```

The default suite is offline. The tests that send real HTTP requests need httpfs, which is not part of this build,
and are skipped unless these environment variables are set:

```bash
python3 test/fixture_server.py 8766 &   # a local OpenAI-compatible fixture API
export OPEN_PROMPT_HTTPFS=httpfs        # or the path of an httpfs.duckdb_extension built for this DuckDB
export OPEN_PROMPT_FIXTURE_URL=http://127.0.0.1:8766
./build/release/test/unittest "test/sql/*"
```

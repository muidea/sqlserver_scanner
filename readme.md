# sqlserver_scanner

DuckDB read-only scanner for Microsoft SQL Server through unixODBC/Windows ODBC.

## Build

```bash
make release
make test_release
```

## Load and attach

Use a DuckDB secret so credentials are redacted from catalog output:

```sql
LOAD sqlserver_scanner;

CREATE SECRET sqlserver_source (
    TYPE sqlserver,
    DRIVER 'FreeTDS',
    SERVER 'sqlserver.example',
    PORT '1433',
    DATABASE 'application',
    UID 'reader',
    PWD 'secret'
);

ATTACH '' AS mssql (
    TYPE sqlserver_scanner,
    READ_ONLY,
    SECRET sqlserver_source,
    SCHEMA 'dbo'
);

SELECT * FROM mssql.dbo.example_table;
SELECT * FROM sqlserver_metadata('mssql', 'dbo');
SELECT * FROM sqlserver_query('mssql', 'SELECT TOP 10 * FROM dbo.example_table');
```

`sqlserver_query` accepts exactly one read-only `SELECT` or `WITH` statement. The catalog rejects create,
insert, update, delete, index, and schema mutations.

## Linux driver

The extension links to unixODBC. Install a SQL Server ODBC driver separately, for example FreeTDS or Microsoft
ODBC Driver 18, and use its registered driver name in the secret.

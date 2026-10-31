# Fixtures (not checked in)

Place captured vendor API responses here for the mock server to serve verbatim.
Not committed ON PURPOSE. Vendor files are too large and would bloat the repo side NEEDLESSLY. So DON'T commit any here!!

Currently expected:

- `endpoint01_websites_all.br` — a real response from ENDPOINT01's `GET /Websites/all` - one shot send compressed result 

- `endpoint02_catalogue.jsonl` — one JSON object per line, each the body of a single item from Endpoint02's `GET /marketplace/catalogue` `data` array. The mock splits this into `endpoint02_pages/page_<N>.jsonl` chunks on first request and serves them back cursor by cursor.

- `endpoint03_catalogue.jsonl` — one JSON object per line, each the body of a single item from Endpoint03's `GET /marketplace` `data` array. The mock splits this into `endpoint03_pages/page_<N>.jsonl` chunks on first request and serves them back page by page.

- `endpoint04_catalogue.jsonl` — one JSON object per line, each the body of a single item from Endpoint04's `POST /marketplace/search` `results` array. The mock splits this into `endpoint04_pages/page_<N>.jsonl` chunks of 100 (endpoint04_import.pl's hardcoded, non-configurable page size) on first request and serves them back page by page.

- `endpoint05_catalogue.jsonl` — one JSON object per line, each the body of a single item from Endpoint05's `GET /marketplace` `data` array. The mock splits this into `endpoint05_pages/page_<N>.jsonl` chunks of 50 (Endpoint05's vendor-fixed, non-configurable page size) on first request and serves them back cursor by cursor, same shape as Endpoint02's.

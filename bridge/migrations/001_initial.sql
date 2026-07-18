CREATE TABLE IF NOT EXISTS webhook_events (
  id bigserial PRIMARY KEY,
  kind text NOT NULL CHECK (kind IN ('98','99')),
  call_id text NOT NULL,
  line text,
  from_number text,
  to_number text,
  status text,
  occurred_at timestamptz NOT NULL,
  raw jsonb NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now(),
  UNIQUE(kind, call_id, occurred_at)
);
CREATE TABLE IF NOT EXISTS ivr_scripts (
  slot smallint PRIMARY KEY CHECK (slot BETWEEN 1 AND 99),
  script text NOT NULL,
  updated_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS recordings (
  id text PRIMARY KEY,
  call_id text NOT NULL,
  object_key text NOT NULL UNIQUE,
  mime text NOT NULL,
  size bigint NOT NULL CHECK (size >= 0),
  metadata jsonb NOT NULL,
  created_at timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS jobs (
  id bigserial PRIMARY KEY,
  kind text NOT NULL,
  payload jsonb NOT NULL,
  attempts integer NOT NULL DEFAULT 0,
  run_after timestamptz NOT NULL DEFAULT now(),
  completed_at timestamptz,
  last_error text
);
CREATE INDEX IF NOT EXISTS jobs_pending ON jobs(run_after) WHERE completed_at IS NULL;

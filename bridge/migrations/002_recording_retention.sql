ALTER TABLE recordings ADD COLUMN IF NOT EXISTS notes text NOT NULL DEFAULT '';
ALTER TABLE recordings ADD COLUMN IF NOT EXISTS retain_until timestamptz;
CREATE INDEX IF NOT EXISTS recordings_retention ON recordings(retain_until) WHERE retain_until IS NOT NULL;

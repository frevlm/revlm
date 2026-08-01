-- token_hash is sha256 hex (64 chars) stored as TEXT; MySQL needs a key-length
-- prefix for indexes on TEXT columns. 64 covers the full value.
CREATE UNIQUE INDEX idx_user_tokens_hash ON user_tokens (token_hash(64));
CREATE INDEX idx_requests_user ON requests (user_id, id);
-- requests.time stores MySQL datetime strings (<=26 chars); prefix is lossless.
CREATE INDEX idx_requests_time ON requests (time(32));
CREATE INDEX idx_requests_token ON requests (token_id);

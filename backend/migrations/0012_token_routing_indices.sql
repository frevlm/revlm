CREATE UNIQUE INDEX idx_user_tokens_hash ON user_tokens (token_hash);
CREATE INDEX idx_requests_user ON requests (user_id, id);
CREATE INDEX idx_requests_time ON requests (time);
CREATE INDEX idx_requests_token ON requests (token_id);

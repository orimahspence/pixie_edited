DROP TYPE vizier_status;
CREATE TYPE vizier_status AS ENUM ('UNKNOWN', 'HEALTHY', 'UNHEALTHY', 'DISCONNECTED', 'UPDATING', 'CONNECTED');

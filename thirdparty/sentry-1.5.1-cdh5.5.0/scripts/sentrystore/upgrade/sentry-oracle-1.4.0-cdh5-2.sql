--Licensed to the Apache Software Foundation (ASF) under one or more
--contributor license agreements.  See the NOTICE file distributed with
--this work for additional information regarding copyright ownership.
--The ASF licenses this file to You under the Apache License, Version 2.0
--(the "License"); you may not use this file except in compliance with
--the License.  You may obtain a copy of the License at
--
--    http://www.apache.org/licenses/LICENSE-2.0
--
--Unless required by applicable law or agreed to in writing, software
--distributed under the License is distributed on an "AS IS" BASIS,
--WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
--See the License for the specific language governing permissions and
--limitations under the License.

CREATE TABLE "SENTRY_DB_PRIVILEGE" (
  "DB_PRIVILEGE_ID" NUMBER NOT NULL,
  "PRIVILEGE_SCOPE" VARCHAR2(32) NOT NULL,
  "SERVER_NAME" VARCHAR2(128) NOT NULL,
  "DB_NAME" VARCHAR2(128) DEFAULT '__NULL__',
  "TABLE_NAME" VARCHAR2(128) DEFAULT '__NULL__',
  "URI" VARCHAR2(4000) DEFAULT '__NULL__',
  "ACTION" VARCHAR2(128) NOT NULL,
  "CREATE_TIME" NUMBER NOT NULL,
  "WITH_GRANT_OPTION" CHAR(1) NOT NULL
);

CREATE TABLE "SENTRY_ROLE" (
  "ROLE_ID" NUMBER  NOT NULL,
  "ROLE_NAME" VARCHAR2(128) NOT NULL,
  "CREATE_TIME" NUMBER NOT NULL
);

CREATE TABLE "SENTRY_GROUP" (
  "GROUP_ID" NUMBER  NOT NULL,
  "GROUP_NAME" VARCHAR2(128) NOT NULL,
  "CREATE_TIME" NUMBER NOT NULL
);

CREATE TABLE "SENTRY_ROLE_DB_PRIVILEGE_MAP" (
  "ROLE_ID" NUMBER NOT NULL,
  "DB_PRIVILEGE_ID" NUMBER NOT NULL,
  "GRANTOR_PRINCIPAL" VARCHAR2(128)
);

CREATE TABLE "SENTRY_ROLE_GROUP_MAP" (
  "ROLE_ID" NUMBER NOT NULL,
  "GROUP_ID" NUMBER NOT NULL,
  "GRANTOR_PRINCIPAL" VARCHAR2(128)
);

CREATE TABLE "SENTRY_VERSION" (
  "VER_ID" NUMBER NOT NULL,
  "SCHEMA_VERSION" VARCHAR(127) NOT NULL,
  "VERSION_COMMENT" VARCHAR(255) NOT NULL
);

ALTER TABLE "SENTRY_DB_PRIVILEGE"
  ADD CONSTRAINT "SENTRY_DB_PRIV_PK" PRIMARY KEY ("DB_PRIVILEGE_ID");

ALTER TABLE "SENTRY_ROLE"
  ADD CONSTRAINT "SENTRY_ROLE_PK" PRIMARY KEY ("ROLE_ID");

ALTER TABLE "SENTRY_GROUP"
  ADD CONSTRAINT "SENTRY_GROUP_PK" PRIMARY KEY ("GROUP_ID");

ALTER TABLE "SENTRY_VERSION" ADD CONSTRAINT "SENTRY_VERSION_PK" PRIMARY KEY ("VER_ID");

ALTER TABLE "SENTRY_DB_PRIVILEGE"
  ADD CONSTRAINT "SENTRY_DB_PRIV_PRIV_NAME_UNIQ" UNIQUE ("SERVER_NAME","DB_NAME","TABLE_NAME","URI","ACTION","WITH_GRANT_OPTION");

CREATE INDEX "SENTRY_SERV_PRIV_IDX" ON "SENTRY_DB_PRIVILEGE" ("SERVER_NAME");

CREATE INDEX "SENTRY_DB_PRIV_IDX" ON "SENTRY_DB_PRIVILEGE" ("DB_NAME");

CREATE INDEX "SENTRY_TBL_PRIV_IDX" ON "SENTRY_DB_PRIVILEGE" ("TABLE_NAME");

CREATE INDEX "SENTRY_URI_PRIV_IDX" ON "SENTRY_DB_PRIVILEGE" ("URI");

ALTER TABLE "SENTRY_ROLE"
  ADD CONSTRAINT "SENTRY_ROLE_ROLE_NAME_UNIQUE" UNIQUE ("ROLE_NAME");

ALTER TABLE "SENTRY_GROUP"
  ADD CONSTRAINT "SENTRY_GRP_GRP_NAME_UNIQUE" UNIQUE ("GROUP_NAME");

ALTER TABLE "SENTRY_ROLE_DB_PRIVILEGE_MAP"
  ADD CONSTRAINT "SEN_RLE_PRIV_MAP_PK" PRIMARY KEY ("ROLE_ID","DB_PRIVILEGE_ID");

ALTER TABLE "SENTRY_ROLE_GROUP_MAP"
  ADD CONSTRAINT "SENTRY_ROLE_GROUP_MAP_PK" PRIMARY KEY ("ROLE_ID","GROUP_ID");

ALTER TABLE "SENTRY_ROLE_DB_PRIVILEGE_MAP"
  ADD CONSTRAINT "SEN_RLE_DB_PRV_MAP_SN_RLE_FK"
  FOREIGN KEY ("ROLE_ID") REFERENCES "SENTRY_ROLE"("ROLE_ID") INITIALLY DEFERRED;

ALTER TABLE "SENTRY_ROLE_DB_PRIVILEGE_MAP"
  ADD CONSTRAINT "SEN_RL_DB_PRV_MAP_SN_DB_PRV_FK"
  FOREIGN KEY ("DB_PRIVILEGE_ID") REFERENCES "SENTRY_DB_PRIVILEGE"("DB_PRIVILEGE_ID") INITIALLY DEFERRED;

ALTER TABLE "SENTRY_ROLE_GROUP_MAP"
  ADD CONSTRAINT "SEN_ROLE_GROUP_MAP_SEN_ROLE_FK"
  FOREIGN KEY ("ROLE_ID") REFERENCES "SENTRY_ROLE"("ROLE_ID") INITIALLY DEFERRED;

ALTER TABLE "SENTRY_ROLE_GROUP_MAP"
  ADD CONSTRAINT "SEN_ROLE_GROUP_MAP_SEN_GRP_FK"
  FOREIGN KEY ("GROUP_ID") REFERENCES "SENTRY_GROUP"("GROUP_ID") INITIALLY DEFERRED;

INSERT INTO SENTRY_VERSION (VER_ID, SCHEMA_VERSION, VERSION_COMMENT) VALUES (1, '1.4.0-cdh5-2', 'Sentry release version 1.4.0-cdh5-2');
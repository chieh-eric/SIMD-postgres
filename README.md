PostgreSQL Database Management System
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this version of PostgreSQL can be found at
<https://www.postgresql.org/docs/devel/>.  In particular, information
about building PostgreSQL from the source code can be found at
<https://www.postgresql.org/docs/devel/installation.html>.

The latest version of this software, and related software, may be
obtained at <https://www.postgresql.org/download/>.  For more information
look at our web site located at <https://www.postgresql.org/>.


Generate Data
=====================================
    CREATE TABLE customer_reviews
    (
        customer_id TEXT,
        review_date DATE,
        review_rating INTEGER,
        review_votes INTEGER,
        review_helpful_votes INTEGER,
        product_id CHAR(10),
        product_title TEXT,
        product_sales_rank BIGINT,
        product_group TEXT,
        product_category TEXT,
        product_subcategory TEXT,
        similar_product_ids CHAR(10)[]
    )

    CREATE TABLE lineitem (
        l_orderkey      integer NOT NULL,
        l_partkey       integer NOT NULL,
        l_suppkey       integer NOT NULL,
        l_linenumber    integer NOT NULL,
        l_quantity      numeric(15,2) NOT NULL,
        l_extendedprice numeric(15,2) NOT NULL,
        l_discount      numeric(15,2) NOT NULL,
        l_tax           numeric(15,2) NOT NULL,
        l_returnflag    character(1) NOT NULL,
        l_linestatus    character(1) NOT NULL,
        l_shipdate      date NOT NULL,
        l_commitdate    date NOT NULL,
        l_receiptdate   date NOT NULL,
        l_shipinstruct  character(25) NOT NULL,
        l_shipmode      character(10) NOT NULL,
        l_comment       character varying(44) NOT NULL
    );


Run Script

    chmod +x generate_and_load.sh

    ./generate_and_load.sh

Build
=====================================

    make distclean 

    ./configure \
    CFLAGS="-O2 -g -mavx2" \
    --without-icu \
    --prefix="$HOME/pginstall" \
    --enable-debug \
    --enable-cassert

    make -j$(nproc)
    make install

Start PostgreSQL server
=====================================

    pg_ctl -D "$HOME/pgdata" stop -m fast

    pg_ctl -D "$HOME/pgdata" -l "$HOME/pgdata/logfile" start

    psql -d postgres

    export PG_FORCE_SIMD_AGG=1


SQL
=====================================
    SET max_parallel_workers_per_gather = 0;

    SELECT sum(l_orderkey) from lineitem;

    SELECT review_rating FROM customer_reviews;

    SELECT sum(review_rating) FROM customer_reviews;

    SELECT max(review_rating) FROM customer_reviews;

    SELECT min(review_rating) FROM customer_reviews;

    SELECT sum(product_sales_rank ) FROM customer_reviews;

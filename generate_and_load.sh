#!/bin/bash

DB=postgres
CHUNKS=5          #
ROWS_PER_CHUNK=1000000
OUTPUT_DIR=/home/vboxuser/SIMD-postgres/reviews_data

if [ -n "$OUTPUT_DIR" ] && [ "$OUTPUT_DIR" != "/" ]; then
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p $OUTPUT_DIR

echo "Generating $((CHUNKS * ROWS_PER_CHUNK)) rows..."

for ((i=1;i<=CHUNKS;i++)); do
    FILE="$OUTPUT_DIR/chunk_$i.csv"
    echo "  - Generating $FILE ..."

    psql -d $DB -c "
        COPY (
            SELECT
                md5(random()::text),
                (date '2000-01-01' + (random()*7000)::int),
                1::int,
                (random()*1000)::int,
                (random()*1000)::int,
                lpad((floor(random()*10000000000))::bigint::text, 10, '0'),
                'Product ' || g,
                (random()*100000)::bigint,
                'Group',
                'Category',
                'Subcategory',
                '{0000000001,0000000002}'::text
            FROM generate_series(1, $ROWS_PER_CHUNK) g
        ) TO '$FILE' WITH (FORMAT CSV);
    "
done

# Combine all CSV into one stream → COPY once (best performance)
echo "Loading into  table using single COPY..."
cat $OUTPUT_DIR/*.csv | psql -d $DB -c \
"COPY customer_reviews FROM STDIN CSV"

echo "Done!"

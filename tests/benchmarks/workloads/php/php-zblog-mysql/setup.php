<?php

declare(strict_types=1);

function setup_environment(string $name, string $default): string
{
    $value = getenv($name);
    return $value === false || $value === '' ? $default : $value;
}

$databaseName = $argv[1] ?? setup_environment('PHP_ZBLOG_MYSQL_DATABASE', 'openasp_benchmark_zblog_mysql');
if (preg_match('/^[A-Za-z0-9_]+$/', $databaseName) !== 1) {
    fwrite(STDERR, "database name must contain only letters, digits, and underscores\n");
    exit(2);
}
$host = setup_environment('PHP_ZBLOG_MYSQL_HOST', '127.0.0.1');
$port = (int)setup_environment('PHP_ZBLOG_MYSQL_PORT', '13316');
$user = setup_environment('PHP_ZBLOG_MYSQL_USER', 'root');
$password = setup_environment('PHP_ZBLOG_MYSQL_PASSWORD', '');
$options = [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_EMULATE_PREPARES => false,
];
$server = new PDO("mysql:host={$host};port={$port};charset=utf8mb4", $user, $password, $options);
$quotedDatabase = '`' . $databaseName . '`';
$server->exec("CREATE DATABASE IF NOT EXISTS {$quotedDatabase} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
$database = new PDO(
    "mysql:host={$host};port={$port};dbname={$databaseName};charset=utf8mb4",
    $user,
    $password,
    $options
);
$database->exec('DROP TABLE IF EXISTS post_tags');
$database->exec('DROP TABLE IF EXISTS posts');
$database->exec('DROP TABLE IF EXISTS tags');
$database->exec(
    'CREATE TABLE posts (
        id INT NOT NULL PRIMARY KEY,
        title VARCHAR(255) NOT NULL,
        slug VARCHAR(255) NOT NULL UNIQUE,
        summary TEXT NOT NULL,
        body TEXT NOT NULL,
        published_at DATETIME NOT NULL,
        views INT NOT NULL,
        KEY posts_published_at (published_at DESC, id DESC)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4'
);
$database->exec(
    'CREATE TABLE tags (
        id INT NOT NULL PRIMARY KEY,
        name VARCHAR(255) NOT NULL UNIQUE
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4'
);
$database->exec(
    'CREATE TABLE post_tags (
        post_id INT NOT NULL,
        tag_id INT NOT NULL,
        PRIMARY KEY (post_id, tag_id),
        KEY post_tags_tag_id (tag_id, post_id),
        CONSTRAINT post_tags_post_fk FOREIGN KEY (post_id) REFERENCES posts(id),
        CONSTRAINT post_tags_tag_fk FOREIGN KEY (tag_id) REFERENCES tags(id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4'
);

$tagNames = ['ASP', 'C', 'database', 'deployment', 'FastCGI', 'Linux', 'macOS', 'performance', 'PHP', 'runtime', 'SQLite', 'web'];
$insertTag = $database->prepare('INSERT INTO tags(id, name) VALUES(:id, :name)');
foreach ($tagNames as $index => $name) {
    $insertTag->execute(['id' => $index + 1, 'name' => $name]);
}

$insertPost = $database->prepare(
    'INSERT INTO posts(id, title, slug, summary, body, published_at, views)
     VALUES(:id, :title, :slug, :summary, :body, :published_at, :views)'
);
$insertPostTag = $database->prepare('INSERT INTO post_tags(post_id, tag_id) VALUES(:post_id, :tag_id)');

$database->beginTransaction();
for ($id = 1; $id <= 120; $id++) {
    $topic = $tagNames[$id % count($tagNames)];
    $summary = "Article {$id} examines {$topic}, request processing, caching, and practical deployment behavior.";
    $body = $summary . "\n\n" .
        'The benchmark executes prepared database queries, formats structured records, escapes dynamic text, and renders shared templates.' . "\n\n" .
        'Each page also loads tag and archive metadata so the request shape resembles a traditional server-rendered blog.' . "\n\n" .
        'The workload intentionally avoids artificial memory allocation and keeps all generated content deterministic.';
    $date = (new DateTimeImmutable('2026-10-07'))->modify('-' . ($id - 1) . ' days')->format('Y-m-d H:i:s');
    $insertPost->execute([
        'id' => $id,
        'title' => "Building a reliable {$topic} service, part {$id}",
        'slug' => "reliable-{$id}",
        'summary' => $summary,
        'body' => $body,
        'published_at' => $date,
        'views' => 100 + (($id * 7919) % 50000),
    ]);
    $tags = [($id % count($tagNames)) + 1, (($id * 5 + 3) % count($tagNames)) + 1, (($id * 7 + 1) % count($tagNames)) + 1];
    foreach (array_unique($tags) as $tagId) {
        $insertPostTag->execute(['post_id' => $id, 'tag_id' => $tagId]);
    }
}
$database->commit();
$database->exec('ANALYZE TABLE posts, tags, post_tags');

printf("created %s on %s:%d with 120 posts\n", $databaseName, $host, $port);

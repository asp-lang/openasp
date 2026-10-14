<?php

declare(strict_types=1);

$path = $argv[1] ?? '';
if ($path === '') {
    fwrite(STDERR, "usage: php setup.php DATABASE\n");
    exit(2);
}

@unlink($path);
$database = new PDO('sqlite:' . $path, null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
]);
$database->exec(
    'CREATE TABLE posts (
        id INTEGER PRIMARY KEY,
        title TEXT NOT NULL,
        slug TEXT NOT NULL UNIQUE,
        summary TEXT NOT NULL,
        body TEXT NOT NULL,
        published_at TEXT NOT NULL,
        views INTEGER NOT NULL
    );
    CREATE TABLE tags (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE
    );
    CREATE TABLE post_tags (
        post_id INTEGER NOT NULL,
        tag_id INTEGER NOT NULL,
        PRIMARY KEY (post_id, tag_id)
    );
    CREATE INDEX posts_published_at ON posts(published_at DESC);
    CREATE INDEX post_tags_tag_id ON post_tags(tag_id, post_id);'
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
$database->exec('ANALYZE');

printf("created %s with 120 posts\n", $path);


<?php

declare(strict_types=1);

require __DIR__ . '/bootstrap.php';

$redis = blog_redis();
$cache = blog_memcache();
$redis->flushDB();
$cache->flush();

$tagNames = ['ASP', 'C', 'database', 'deployment', 'FastCGI', 'Linux', 'macOS', 'performance', 'PHP', 'runtime', 'SQLite', 'web'];
$tagPosts = array_fill_keys(array_map('strtolower', $tagNames), []);
$tagCounts = array_fill_keys($tagNames, 0);
$archives = [];

for ($id = 1; $id <= 120; $id++) {
    $topic = $tagNames[$id % count($tagNames)];
    $summary = "Article {$id} examines {$topic}, request processing, caching, and practical deployment behavior.";
    $body = $summary . "\n\n" .
        'The benchmark executes prepared database queries, formats structured records, escapes dynamic text, and renders shared templates.' . "\n\n" .
        'Each page also loads tag and archive metadata so the request shape resembles a traditional server-rendered blog.' . "\n\n" .
        'The workload intentionally avoids artificial memory allocation and keeps all generated content deterministic.';
    $date = (new DateTimeImmutable('2026-10-07'))->modify('-' . ($id - 1) . ' days')->format('Y-m-d H:i:s');
    $post = implode("\t", [
        (string)$id,
        "Building a reliable {$topic} service, part {$id}",
        "reliable-{$id}",
        $summary,
        $body,
        $date,
        (string)(100 + (($id * 7919) % 50000)),
    ]);
    $redis->set("zblog:post:{$id}", $post);
    $redis->rPush('zblog:posts', (string)$id);

    $tagIds = array_values(array_unique([
        ($id % count($tagNames)) + 1,
        (($id * 5 + 3) % count($tagNames)) + 1,
        (($id * 7 + 1) % count($tagNames)) + 1,
    ]));
    $postTags = [];
    foreach ($tagIds as $tagId) {
        $name = $tagNames[$tagId - 1];
        $postTags[] = $name;
        $tagPosts[strtolower($name)][] = (string)$id;
        $tagCounts[$name]++;
    }
    sort($postTags, SORT_STRING);
    $cache->set("zblog:posttags:{$id}", implode(',', $postTags));

    $month = substr($date, 0, 7);
    $archives[$month] = ($archives[$month] ?? 0) + 1;
    $searchText = strtolower("Building a reliable {$topic} service, part {$id} {$summary} {$body}");
    foreach (array_unique(array_map('strtolower', $tagNames)) as $term) {
        if (str_contains($searchText, $term)) {
            $redis->rPush("zblog:search:{$term}", (string)$id);
        }
    }
}

foreach ($tagPosts as $name => $ids) {
    if ($ids !== []) {
        $redis->rPush("zblog:tag:{$name}", ...$ids);
    }
}

uksort($tagCounts, static fn(string $left, string $right): int =>
    $tagCounts[$right] <=> $tagCounts[$left] ?: strcmp($left, $right)
);
$tagMetadata = [];
foreach ($tagCounts as $name => $count) {
    $tagMetadata[] = "{$name}\t{$count}";
}
$cache->set('zblog:tags', implode("\n", $tagMetadata));

krsort($archives, SORT_STRING);
$archiveMetadata = [];
foreach (array_slice($archives, 0, 12, true) as $month => $count) {
    $archiveMetadata[] = "{$month}\t{$count}";
}
$cache->set('zblog:archives', implode("\n", $archiveMetadata));

printf("created Redis/Memcache fixture with 120 posts\n");

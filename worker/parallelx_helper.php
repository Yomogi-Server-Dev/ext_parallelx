<?php
declare(strict_types=1);

namespace ParallelX\Helper;

use ReflectionFunction;

/**
 * クロージャをSerializableな文字列とSerializeされたバインド変数として抽出
 * Returns ['source' => string (e.g. 'function($a,$b) use($x){...}'), 'bound_b64' => base64(serialized array)].
 */
function extract_closure_descriptor(\Closure $c): array {
    $rf = new ReflectionFunction($c);
    $file = $rf->getFileName();
    $start = $rf->getStartLine();
    $end = $rf->getEndLine();

    $lines = @file($file, FILE_IGNORE_NEW_LINES);
    if ($lines === false) {
        throw new \RuntimeException("parallelx_helper: cannot read closure source file: $file");
    }
    $start = max(1, $start);
    $end = min(count($lines), $end);
    $chunk = array_slice($lines, $start - 1, $end - $start + 1);
    $source = implode("\n", $chunk);

    $statics = $rf->getStaticVariables();
    $bound_b64 = '';
    if (!empty($statics)) {
        $bound_b64 = base64_encode(serialize($statics));
    }

    return ['source' => $source, 'bound_b64' => $bound_b64];
}

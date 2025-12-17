<?php

$autoload = getenv('PARALLELX_AUTOLOAD');
if ($autoload !== false && file_exists($autoload)) {
    @require_once $autoload;
}

while (!feof(STDIN)) {
    $len_bytes = fread(STDIN, 4);
    if ($len_bytes === false || strlen($len_bytes) < 4) break;
    $arr = unpack('Nlen', $len_bytes);
    $len = $arr['len'];
    $data = '';
    $remain = $len;
    while ($remain > 0 && !feof(STDIN)) {
        $chunk = fread(STDIN, $remain);
        if ($chunk === false) break;
        $data .= $chunk;
        $remain = $len - strlen($data);
    }
    if ($data === '') continue;
    $desc = json_decode($data, true);
    if (!is_array($desc)) {
        $out = ['task_id'=>0, 'success'=>false, 'data'=>'invalid descriptor'];
    } else {
        $tid = $desc['task_id'] ?? 0;
        try {
            if (($desc['type'] ?? '') === 'closure_exec') {
                $src = $desc['source'] ?? '';
                $bound_b64 = $desc['bound_b64'] ?? '';
                $args = $desc['args'] ?? [];
                if ($bound_b64 !== '') {
                    $bound = @unserialize(base64_decode($bound_b64));
                    if (is_array($bound)) extract($bound);
                }
                ob_start();
                $closure = eval('return ' . $src . ';');
                if (!is_callable($closure)) {
                    $out = ['task_id'=>$tid,'success'=>false,'data'=>'eval did not return callable'];
                } else {
                    $ret = call_user_func_array($closure, $args);
                    $outbuf = ob_get_clean();
                    $payload = ['return'=>$ret, 'output'=>$outbuf];
                    $out = ['task_id'=>$tid,'success'=>true,'data'=>base64_encode(serialize($payload))];
                }
            } else {
                $out = ['task_id'=>$tid,'success'=>false,'data'=>'unknown type'];
            }
        } catch (Throwable $e) {
            $out = ['task_id'=>$tid,'success'=>false,'data'=>'exception: '.$e->getMessage()];
        }
    }
    $json = json_encode($out);
    $len2 = strlen($json);
    echo pack('N', $len2) . $json;
    fflush(STDOUT);
}
exit(0);

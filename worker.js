const TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJqdGkiOiI2OWVmNzAwZTc4ZWQ4OTVhMmI3OGQ2YmIiLCJzdWIiOiI2OWU2MjRmNTc4ZWQ4OTVhMmI3OGQ1MjMiLCJncnAiOiI2OWU2MjRmNTc4ZWQ4OTVhMmI3OGQ1MjIiLCJvcmciOiI2OWU2MjRmNTc4ZWQ4OTVhMmI3OGQ1MjIiLCJsaWMiOiI2ODc2NDNlMGNiMWI2Y2Y1MjA4YjJiNmMiLCJ1c2ciOiJhcGkiLCJmdWxsIjpmYWxzZSwicmlnaHRzIjoxLjUsImlhdCI6MTc3NzI5OTQ3MCwiZXhwIjoxODQyOTgwNDAwfQ.7rFmM6hHPEbDuw3ejmWXr0L0_G_T5utbAehTcnCznbs";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    
    const corsHeaders = {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type, Authorization',
    };

    if (request.method === 'OPTIONS') {
      return new Response(null, { headers: corsHeaders });
    }

    // POST /webhook — приём данных от Rightech
    if (request.method === 'POST' && url.pathname === '/webhook') {
      const auth = request.headers.get('Authorization');
      if (auth && auth !== `Bearer ${TOKEN}`) {
        return new Response('Unauthorized', { status: 401, headers: corsHeaders });
      }

      try {
        const data = await request.json();
        console.log('Received:', JSON.stringify(data));
        
        // Получаем текущее состояние из KV
        let deviceState = await env.DIAPER_STATE.get('deviceState', 'json');
        if (!deviceState) {
          deviceState = { accel: 0, micro: 0, water: 0, temp: 0 };
        }
        
        // Обновляем состояние
        if (data.params) {
          if (typeof data.params.accel === 'number') deviceState.accel = data.params.accel;
          if (typeof data.params.micro === 'number') deviceState.micro = data.params.micro;
          if (typeof data.params.water === 'number') deviceState.water = data.params.water;
          if (typeof data.params.temp === 'number') if(data.params.temp !== 0) deviceState.temp = data.params.temp+3.5;
        }
        
        // Также проверяем плоский формат
        if (typeof data.accel === 'number') deviceState.accel = data.accel;
        if (typeof data.micro === 'number') deviceState.micro = data.micro;
        if (typeof data.water === 'number') deviceState.water = data.water;
        if (typeof data.temp === 'number') if(data.temp !== 0) deviceState.temp = data.temp+3.5;
        
        // Добавляем timestamp последнего обновления
        deviceState.lastUpdate = Date.now();
        
        // Сохраняем в KV
        await env.DIAPER_STATE.put('deviceState', JSON.stringify(deviceState));
        
        console.log('State saved to KV:', deviceState);
        
        return new Response(JSON.stringify({ success: true, state: deviceState }), {
          headers: { ...corsHeaders, 'Content-Type': 'application/json' }
        });
        
      } catch (e) {
        console.error('Webhook error:', e);
        return new Response(JSON.stringify({ error: 'Invalid JSON', details: e.message }), {
          status: 400,
          headers: { ...corsHeaders, 'Content-Type': 'application/json' }
        });
      }
    }

    // GET /status — отдаём состояние
    if (request.method === 'GET' && url.pathname === '/status') {
      // Получаем состояние из KV
      const deviceState = await env.DIAPER_STATE.get('deviceState', 'json');
      const defaultState = { accel: 0, micro: 0, water: 0, temp: 0};
      
      return new Response(JSON.stringify(deviceState || defaultState), {
        headers: { ...corsHeaders, 'Content-Type': 'application/json' }
      });
    }

    // GET / — тестовая страница
    if (request.method === 'GET' && url.pathname === '/') {
      const deviceState = await env.DIAPER_STATE.get('deviceState', 'json');
      const defaultState = { accel: 0, micro: 0, water: 0, temp: 0 };
      
      const html = `<!DOCTYPE html>
      <html>
      <head>
        <title>Smart Diaper Gateway</title>
        <meta charset="UTF-8">
        <style>
          body { font-family: monospace; padding: 20px; }
          pre { background: #f0f0f0; padding: 10px; border-radius: 5px; overflow-x: auto; }
          .status { color: green; font-weight: bold; }
          .value { font-size: 24px; font-weight: bold; margin: 10px 0; }
        </style>
      </head>
      <body>
        <h2>Smart Diaper Gateway (with KV Storage)</h2>
        <p class="status">Worker is running with persistent storage!</p>
        <hr>
        <h3>API Endpoints:</h3>
        <ul>
          <li><b>POST</b> ${url.origin}/webhook - receive data from Rightech</li>
          <li><b>GET</b> ${url.origin}/status - get current state</li>
        </ul>
        <hr>
        <h3>Test Form:</h3>
        <form id="testForm">
          <label>Water (0/1): <input type="number" id="water" step="1" value="0"></label><br>
          <label>Micro (0/1): <input type="number" id="micro" step="1" value="0"></label><br>
          <label>Accel (0/1): <input type="number" id="accel" step="1" value="0"></label><br>
          <label>Temp (any): <input type="number" id="temp" step="1" value="0"></label><br>
          <button type="submit">Send Test</button>
        </form>
        <pre id="result"></pre>
        <script>
          document.getElementById('testForm').onsubmit = async (e) => {
            e.preventDefault();
            const data = {
              params: {
                water: parseInt(document.getElementById('water').value) || 0,
                micro: parseInt(document.getElementById('micro').value) || 0,
                accel: parseInt(document.getElementById('accel').value) || 0,
                temp: parseFloat(document.getElementById('temp').value) || 0
              }
            };
            const res = await fetch('${url.origin}/webhook', {
              method: 'POST',
              headers: {'Content-Type': 'application/json'},
              body: JSON.stringify(data)
            });
            const result = await res.json();
            document.getElementById('result').textContent = JSON.stringify(result, null, 2);
            setTimeout(() => location.reload(), 1000);
          };
        </script>
      </body>
      </html>`;
      return new Response(html, {
        headers: { ...corsHeaders, 'Content-Type': 'text/html' }
      });
    }

    return new Response('Not Found', { status: 404, headers: corsHeaders });
  }
};
/**
 * Distributed LSM-Tree Database Engine - Web GUI Application Logic
 * Pure vanilla JavaScript - no external dependencies.
 */

document.addEventListener('DOMContentLoaded', () => {
  // --- DOM Elements ---
  const connIndicator = document.getElementById('connIndicator');
  const connLabel = document.getElementById('connLabel');
  const valLeader = document.getElementById('valLeader');
  const valTerm = document.getElementById('valTerm');
  const autoRefreshToggle = document.getElementById('autoRefreshToggle');
  const nodeGrid = document.getElementById('nodeGrid');
  const statusBanner = document.getElementById('statusBanner');

  const btnRefresh = document.getElementById('btnRefresh');
  const btnStartCluster = document.getElementById('btnStartCluster');
  const btnStopCluster = document.getElementById('btnStopCluster');

  const tabBtns = document.querySelectorAll('.tab-btn');
  const kvForm = document.getElementById('kvForm');
  const targetEndpoint = document.getElementById('targetEndpoint');
  const kvKey = document.getElementById('kvKey');
  const kvValue = document.getElementById('kvValue');
  const groupValue = document.getElementById('groupValue');
  const btnSubmitKv = document.getElementById('btnSubmitKv');
  const btnSubmitText = document.getElementById('btnSubmitText');
  const btnSubmitSpinner = document.getElementById('btnSubmitSpinner');
  const btnClearForm = document.getElementById('btnClearForm');

  const resStatusBadge = document.getElementById('resStatusBadge');
  const resTarget = document.getElementById('resTarget');
  const resHandling = document.getElementById('resHandling');
  const resLatency = document.getElementById('resLatency');
  const resContent = document.getElementById('resContent');
  const btnToggleRaw = document.getElementById('btnToggleRaw');
  const rawOutput = document.getElementById('rawOutput');

  const activityCount = document.getElementById('activityCount');
  const activityTableBody = document.getElementById('activityTableBody');

  // --- State ---
  let activeOp = 'put';
  let refreshTimer = null;
  let isFetchingStatus = false;

  // --- Helpers ---
  function showBanner(message, type = 'info', duration = 4000) {
    statusBanner.className = `alert-banner ${type}`;
    statusBanner.textContent = message;
    statusBanner.classList.remove('hidden');
    if (duration > 0) {
      setTimeout(() => {
        statusBanner.classList.add('hidden');
      }, duration);
    }
  }

  function hideBanner() {
    statusBanner.classList.add('hidden');
  }

  // --- Tab Switching ---
  tabBtns.forEach(btn => {
    btn.addEventListener('click', (e) => {
      e.preventDefault();
      tabBtns.forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      activeOp = btn.dataset.op;

      btnSubmitText.textContent = `Execute ${activeOp.toUpperCase()}`;
      if (activeOp === 'get' || activeOp === 'delete') {
        groupValue.classList.add('hidden');
        kvValue.removeAttribute('required');
      } else {
        groupValue.classList.remove('hidden');
        kvValue.setAttribute('required', 'required');
      }
    });
  });

  // --- Clear Form ---
  btnClearForm.addEventListener('click', () => {
    kvKey.value = '';
    kvValue.value = '';
    rawOutput.classList.add('hidden');
    rawOutput.textContent = '';
    resStatusBadge.textContent = '-';
    resStatusBadge.className = 'badge';
    resTarget.textContent = '-';
    resHandling.textContent = '-';
    resLatency.textContent = '-';
    resContent.textContent = 'Submit an operation to see responses.';
  });

  // --- Raw Toggle ---
  btnToggleRaw.addEventListener('click', () => {
    if (rawOutput.classList.contains('hidden')) {
      rawOutput.classList.remove('hidden');
      btnToggleRaw.textContent = 'Hide raw output';
    } else {
      rawOutput.classList.add('hidden');
      btnToggleRaw.textContent = 'View raw output';
    }
  });

  // --- Cluster Status Fetching ---
  async function fetchClusterStatus() {
    if (isFetchingStatus) return;
    isFetchingStatus = true;

    try {
      const res = await fetch('/api/status', { cache: 'no-store' });
      if (!res.ok) throw new Error(`HTTP error ${res.status}`);
      const data = await res.json();
      renderClusterStatus(data);
    } catch (err) {
      connIndicator.className = 'connection-indicator offline';
      connLabel.textContent = 'Backend Adapter Disconnected';
      valLeader.textContent = 'Offline';
      valTerm.textContent = '-';
    } finally {
      isFetchingStatus = false;
    }
  }

  function renderClusterStatus(data) {
    const nodes = data.nodes || [];
    const healthyCount = nodes.filter(n => n.status === 'healthy').length;

    if (healthyCount > 0) {
      connIndicator.className = 'connection-indicator online';
      connLabel.textContent = `Cluster Online (${healthyCount}/${nodes.length} Nodes)`;
    } else {
      connIndicator.className = 'connection-indicator offline';
      connLabel.textContent = 'Cluster Offline (0 Nodes)';
    }

    valLeader.textContent = data.leader_id ? `Node ${data.leader_id}` : (healthyCount > 0 ? 'Electing...' : 'None');
    valTerm.textContent = data.current_term !== null ? data.current_term : '-';

    // Render Node Cards
    nodeGrid.innerHTML = '';
    nodes.forEach(node => {
      const isLeader = node.role === 'LEADER';
      const isHealthy = node.status === 'healthy';

      const card = document.createElement('div');
      card.className = `node-card ${isLeader ? 'leader' : ''} ${!isHealthy ? 'offline' : ''}`;

      let badgeHtml = '';
      if (!isHealthy) {
        badgeHtml = `<span class="badge badge-offline">Offline</span>`;
      } else if (isLeader) {
        badgeHtml = `<span class="badge badge-leader">Leader</span>`;
      } else {
        badgeHtml = `<span class="badge badge-follower">Follower</span>`;
      }

      const nodeIdDisplay = node.configured_id || node.node_id || '?';
      const peerSummary = (node.peers && node.peers.length > 0)
        ? node.peers.map(p => `Node ${p.node_id} (${p.host}:${p.port})`).join(', ')
        : (isHealthy ? 'No registered peers' : 'Unreachable');

      const toggleBtnHtml = isHealthy
        ? `<button class="btn-node-toggle" data-node="${nodeIdDisplay}" data-action="stop">Stop Node</button>`
        : `<button class="btn-node-toggle" data-node="${nodeIdDisplay}" data-action="start">Start Node</button>`;

      card.innerHTML = `
        <div class="node-card-header">
          <div class="node-title-group">
            <span class="node-name">Node ${nodeIdDisplay}</span>
            <span class="panel-badge">${node.endpoint}</span>
          </div>
          ${badgeHtml}
        </div>
        <div class="node-card-body">
          <div class="stat-row">
            <span class="stat-label">Health</span>
            <span class="stat-val" style="color: ${isHealthy ? 'var(--accent-forest)' : 'var(--color-error)'}">${isHealthy ? 'ONLINE' : 'UNREACHABLE'}</span>
          </div>
          <div class="stat-row">
            <span class="stat-label">Raft Role</span>
            <span class="stat-val">${isHealthy ? node.role : 'N/A'}</span>
          </div>
          <div class="stat-row">
            <span class="stat-label">Raft Term</span>
            <span class="stat-val">${isHealthy ? (node.term !== null ? node.term : '-') : 'N/A'}</span>
          </div>
          <div class="stat-row">
            <span class="stat-label">Commit Index</span>
            <span class="stat-val">${isHealthy ? (node.commit_index !== null ? node.commit_index : '-') : 'N/A'}</span>
          </div>
          <div class="stat-row">
            <span class="stat-label">Last Applied</span>
            <span class="stat-val">${isHealthy ? (node.last_applied !== null ? node.last_applied : '-') : 'N/A'}</span>
          </div>
          <div class="stat-peers">
            Peers: ${peerSummary}
          </div>
        </div>
        <div class="node-card-footer">
          ${toggleBtnHtml}
        </div>
      `;

      const btnToggle = card.querySelector('.btn-node-toggle');
      if (btnToggle) {
        btnToggle.addEventListener('click', async (e) => {
          e.preventDefault();
          const nid = parseInt(e.target.dataset.node);
          const action = e.target.dataset.action;
          e.target.disabled = true;
          showBanner(`${action === 'stop' ? 'Stopping' : 'Starting'} Node ${nid}...`, 'info', 2500);
          try {
            await fetch(`/api/node/${action}`, {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ node_id: nid }),
            });
          } catch (_) {}
          setTimeout(() => {
            fetchClusterStatus();
            fetchActivity();
          }, 1200);
        });
      }

      nodeGrid.appendChild(card);
    });
  }

  // --- Activity Feed Fetching ---
  async function fetchActivity() {
    try {
      const res = await fetch('/api/activity', { cache: 'no-store' });
      if (!res.ok) return;
      const data = await res.json();
      renderActivity(data.events || []);
    } catch (_) {
      // Ignore background activity fetch errors
    }
  }

  function renderActivity(events) {
    activityCount.textContent = `${events.length} event${events.length === 1 ? '' : 's'}`;

    if (events.length === 0) {
      activityTableBody.innerHTML = `
        <tr>
          <td colspan="7" class="empty-state">No operations recorded yet. Use the operations panel to begin.</td>
        </tr>
      `;
      return;
    }

    activityTableBody.innerHTML = '';
    events.forEach(ev => {
      const row = document.createElement('tr');

      let opBadgeClass = 'badge-teal';
      if (ev.op === 'PUT') opBadgeClass = 'badge-teal';
      else if (ev.op === 'GET') opBadgeClass = 'badge-follower';
      else if (ev.op === 'DELETE') opBadgeClass = 'badge-amber';
      else if (ev.op === 'CLUSTER') opBadgeClass = 'badge-neutral';

      let statusBadgeClass = 'badge-leader';
      if (ev.status === 'ERROR' || ev.status === 'UNAVAILABLE' || ev.status === 'STOPPED') {
        statusBadgeClass = 'badge-offline';
      } else if (ev.status === 'NOT_FOUND') {
        statusBadgeClass = 'badge-follower';
      }

      const isRedirected = ev.handling_node && ev.handling_node !== ev.target;
      const handlingDisplay = isRedirected
        ? `<span style="color: var(--accent-amber); font-weight: 600;" title="Redirected from target">${ev.handling_node}</span>`
        : `<span>${ev.handling_node}</span>`;

      row.innerHTML = `
        <td>${ev.timestamp}</td>
        <td><span class="badge ${opBadgeClass}">${ev.op}</span></td>
        <td title="${ev.key}">${ev.key}</td>
        <td>${ev.target}</td>
        <td>${handlingDisplay}</td>
        <td><span class="badge ${statusBadgeClass}">${ev.status}</span></td>
        <td>${ev.duration_ms}ms</td>
      `;

      activityTableBody.appendChild(row);
    });
  }

  // --- KV Operation Submit ---
  kvForm.addEventListener('submit', async (e) => {
    e.preventDefault();

    const key = kvKey.value.trim();
    const value = kvValue.value;
    const target = targetEndpoint.value;

    if (!key) {
      alert('Key is required.');
      return;
    }

    btnSubmitKv.disabled = true;
    btnSubmitSpinner.classList.remove('hidden');

    try {
      const res = await fetch('/api/kv', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          op: activeOp,
          key: key,
          value: value,
          target: target,
        }),
      });

      const result = await res.json();
      displayResult(result);
      fetchActivity();
      fetchClusterStatus();
    } catch (err) {
      displayResult({
        success: false,
        op: activeOp.toUpperCase(),
        key: key,
        value: null,
        status: 'NETWORK_ERROR',
        target: target,
        handling_node: target,
        redirected: false,
        raw_output: err.message,
        duration_ms: 0,
      });
    } finally {
      btnSubmitKv.disabled = false;
      btnSubmitSpinner.classList.add('hidden');
    }
  });

  function displayResult(res) {
    resTarget.textContent = res.target || '-';
    resLatency.textContent = res.duration_ms ? `${res.duration_ms} ms` : '-';
    rawOutput.textContent = res.raw_output || '(no output)';

    if (res.redirected) {
      resHandling.innerHTML = `<span style="color: var(--accent-amber); font-weight: 600;">${res.handling_node} (Auto-Redirected)</span>`;
    } else {
      resHandling.textContent = res.handling_node || '-';
    }

    if (res.success) {
      resStatusBadge.className = 'badge badge-leader';
      resStatusBadge.textContent = res.status;

      if (res.op === 'GET') {
        if (res.status === 'FOUND') {
          resContent.innerHTML = `<strong style="color: var(--accent-forest)">Value:</strong> "${escapeHtml(res.value)}"`;
        } else if (res.status === 'NOT_FOUND') {
          resStatusBadge.className = 'badge badge-follower';
          resContent.innerHTML = `<span style="color: var(--text-muted)">Key "${escapeHtml(res.key)}" was not found in the LSM store.</span>`;
        }
      } else if (res.op === 'PUT') {
        resContent.innerHTML = `<span style="color: var(--accent-forest)">&#10003; Successfully replicated and committed to Raft quorum.</span>`;
      } else if (res.op === 'DELETE') {
        resContent.innerHTML = `<span style="color: var(--accent-forest)">&#10003; Tombstone recorded and committed to Raft quorum.</span>`;
      } else {
        resContent.textContent = res.raw_output || 'Operation completed.';
      }
    } else {
      resStatusBadge.className = 'badge badge-offline';
      resStatusBadge.textContent = res.status || 'ERROR';
      resContent.innerHTML = `<span style="color: var(--color-error)">&#10007; Operation Failed:</span>\n${escapeHtml(res.raw_output || 'Unknown error')}`;
    }
  }

  function escapeHtml(str) {
    if (!str) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#039;');
  }

  // --- Cluster Lifecycle Actions ---
  btnStartCluster.addEventListener('click', async () => {
    btnStartCluster.disabled = true;
    showBanner('Starting 3-node Raft cluster in background...', 'info', 3000);
    try {
      const res = await fetch('/api/cluster/start', { method: 'POST' });
      const data = await res.json();
      if (data.success) {
        showBanner('3-node Raft cluster started successfully.', 'info', 4000);
      } else {
        showBanner(`Cluster start error: ${data.error || 'Failed'}`, 'error', 5000);
      }
    } catch (err) {
      showBanner(`Request failed: ${err.message}`, 'error', 5000);
    } finally {
      btnStartCluster.disabled = false;
      setTimeout(() => {
        fetchClusterStatus();
        fetchActivity();
      }, 1500);
    }
  });

  btnStopCluster.addEventListener('click', async () => {
    btnStopCluster.disabled = true;
    showBanner('Stopping local cluster nodes...', 'info', 3000);
    try {
      const res = await fetch('/api/cluster/stop', { method: 'POST' });
      const data = await res.json();
      if (data.success) {
        showBanner('Cluster processes stopped.', 'info', 4000);
      } else {
        showBanner(`Cluster stop error: ${data.error || 'Failed'}`, 'error', 5000);
      }
    } catch (err) {
      showBanner(`Request failed: ${err.message}`, 'error', 5000);
    } finally {
      btnStopCluster.disabled = false;
      setTimeout(() => {
        fetchClusterStatus();
        fetchActivity();
      }, 1000);
    }
  });

  btnRefresh.addEventListener('click', () => {
    fetchClusterStatus();
    fetchActivity();
  });

  // --- Auto-refresh Management ---
  function updateAutoRefresh() {
    if (autoRefreshToggle.checked) {
      if (!refreshTimer) {
        refreshTimer = setInterval(() => {
          fetchClusterStatus();
          fetchActivity();
        }, 3000);
      }
    } else {
      if (refreshTimer) {
        clearInterval(refreshTimer);
        refreshTimer = null;
      }
    }
  }

  autoRefreshToggle.addEventListener('change', updateAutoRefresh);

  // --- Initial Trigger ---
  fetchClusterStatus();
  fetchActivity();
  updateAutoRefresh();
});

#!/usr/bin/env bash
# Sunday smoke test: deploy the three-node baseline and verify it works.
# Run from the deploy/ directory:  cd deploy && ./smoke-test.sh
set -euo pipefail

CLUSTER_NAME="lsm-dev"
RELEASE_NAME="lsm-db"

echo "== 1. Building the placeholder node image =="
docker build -t lsm-db-node:dev ./placeholder-node

echo "== 2. Loading image into kind cluster =="
kind load docker-image lsm-db-node:dev --name "$CLUSTER_NAME"

echo "== 3. Installing/upgrading the Helm release =="
helm upgrade --install "$RELEASE_NAME" ./helm/lsm-db

echo "== 4. Waiting for all 3 pods to be Ready =="
kubectl rollout status statefulset/"$RELEASE_NAME" --timeout=240s

echo "== 5. Pod status =="
kubectl get pods -l app.kubernetes.io/name=lsm-db -o wide

echo "== 6. Verifying stable pod identities =="
kubectl get pods -l app.kubernetes.io/name=lsm-db -o custom-columns=NAME:.metadata.name

echo "== 7. Verifying headless service DNS resolves per-pod =="
kubectl run dns-check --rm -i --restart=Never --image=busybox -- \
  nslookup "${RELEASE_NAME}-0.${RELEASE_NAME}-headless.default.svc.cluster.local"

echo "== 8. Verifying each node responds on its port =="
for i in 0 1 2; do
  echo "-- ${RELEASE_NAME}-${i} --"
  kubectl run "tcp-check-$i" --rm -i --restart=Never --image=busybox -- \
    sh -c "nc -z -w 3 ${RELEASE_NAME}-${i}.${RELEASE_NAME}-headless.default.svc.cluster.local 50051" \
    || { echo "FAIL: ${RELEASE_NAME}-${i} not reachable via headless DNS"; exit 1; }
  kubectl logs "${RELEASE_NAME}-${i}" --tail=5
done
echo "== 9. Verifying all 3 pods are Ready =="
READY=$(kubectl get statefulset "$RELEASE_NAME" -o jsonpath='{.status.readyReplicas}')
[ "$READY" = "3" ] || { echo "FAIL: expected 3 ready pods, got '$READY'"; exit 1; }
echo "PASS: 3/3 pods ready and reachable"
echo "== Smoke test complete =="

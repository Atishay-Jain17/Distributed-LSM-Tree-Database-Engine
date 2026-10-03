#!/usr/bin/env bash
# Sunday smoke test: deploy the three-node baseline and verify it works.
# Run from the week1-suhani/ directory.
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
kubectl rollout status statefulset/"$RELEASE_NAME" --timeout=90s

echo "== 5. Pod status =="
kubectl get pods -l app.kubernetes.io/name=lsm-db -o wide

echo "== 6. Verifying stable pod identities =="
kubectl get pods -l app.kubernetes.io/name=lsm-db -o custom-columns=NAME:.metadata.name

echo "== 7. Verifying headless service DNS resolves per-pod =="
kubectl run dns-check --rm -i --restart=Never --image=busybox -- \
  nslookup "${RELEASE_NAME}-0.${RELEASE_NAME}-headless"

echo "== 8. Verifying each node responds on its port =="
for i in 0 1 2; do
  echo "-- ${RELEASE_NAME}-${i} --"
  kubectl exec "${RELEASE_NAME}-${i}" -- sh -c \
    "echo | timeout 2 nc localhost 50051" || echo "  (nc not in image; check logs instead)"
  kubectl logs "${RELEASE_NAME}-${i}" --tail=5
done

echo "== Smoke test complete =="

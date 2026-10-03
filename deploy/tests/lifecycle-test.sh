#!/usr/bin/env bash
# Wednesday: lifecycle tests.
# Verifies that a pod restart preserves (1) its identity/name and
# (2) its PersistentVolumeClaim's data, and that the headless service
# still resolves correctly afterward.
#
# Run this AFTER smoke-test.sh has already deployed the cluster.
set -euo pipefail

RELEASE_NAME="lsm-db"
TARGET_POD="${RELEASE_NAME}-1"   # test the middle node

echo "== 1. Write a marker file into the target pod's persistent volume =="
kubectl exec "$TARGET_POD" -- sh -c 'echo "lifecycle-test-marker" > /data/marker.txt'
kubectl exec "$TARGET_POD" -- cat /data/marker.txt

echo "== 2. Record the pod's current UID (to prove it's a NEW pod after restart) =="
OLD_UID=$(kubectl get pod "$TARGET_POD" -o jsonpath='{.metadata.uid}')
echo "Old pod UID: $OLD_UID"

echo "== 3. Delete the pod (simulates a crash) =="
kubectl delete pod "$TARGET_POD"

echo "== 4. Wait for Kubernetes to recreate it with the SAME NAME =="
kubectl wait --for=condition=Ready "pod/${TARGET_POD}" --timeout=60s

NEW_UID=$(kubectl get pod "$TARGET_POD" -o jsonpath='{.metadata.uid}')
echo "New pod UID: $NEW_UID"

if [ "$OLD_UID" == "$NEW_UID" ]; then
  echo "FAIL: pod UID did not change -- it never actually restarted."
  exit 1
fi
echo "PASS: pod was recreated (new UID) but kept the same name ($TARGET_POD)."

echo "== 5. Verify the persistent volume reattached with the marker file intact =="
RECOVERED=$(kubectl exec "$TARGET_POD" -- cat /data/marker.txt)
if [ "$RECOVERED" != "lifecycle-test-marker" ]; then
  echo "FAIL: marker file missing or corrupted after restart. PV did not reattach correctly."
  exit 1
fi
echo "PASS: marker file survived the restart -- PersistentVolumeClaim reattached correctly."

echo "== 6. Verify headless service DNS still resolves to the restarted pod =="
kubectl run dns-check-lifecycle --rm -i --restart=Never --image=busybox -- \
  nslookup "${TARGET_POD}.${RELEASE_NAME}-headless.default.svc.cluster.local"

echo "== 7. Clean up the marker file =="
kubectl exec "$TARGET_POD" -- rm -f /data/marker.txt

echo "== Lifecycle test complete: identity, storage, and DNS all survived a pod restart =="

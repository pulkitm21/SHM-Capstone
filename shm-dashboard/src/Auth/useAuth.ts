import { useAuthContext } from "./AuthContext";

/*
  This wrapper keeps the rest of the app importing a small auth hook
  instead of importing the raw context directly.
*/
export default function useAuth() {
  return useAuthContext();
}
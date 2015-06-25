using System;
using HyperFastCgi.Interfaces;
using System.Collections.Generic;
using HyperFastCgi.Configuration;
using System.Threading;
using System.Diagnostics;
using System.Reflection;


#if !NET_2_0
using System.Threading.Tasks;
#endif 

namespace HyperFastCgi.Transports
{
	public abstract class BaseAppHostTransport : MarshalByRefObject, IApplicationHostTransport
	{
		Dictionary<ulong, IWebRequest> requests = new Dictionary<ulong, IWebRequest> ();
		object requestsLock = new object ();
		IApplicationHost appHost;
		MultiThreadingOption mt;

		public IApplicationHost AppHost {
			get { return appHost; }
		}

		public MultiThreadingOption MultiThreading {
			get { return mt; }
		}

		protected virtual void OnHostUnload (IApplicationHost host, bool isShutdown)
		{

		}

		#region IApplicationHostTransport implementation

		public virtual void Configure (IApplicationHost host, object transportConfig)
		{
			this.appHost = host;
			host.HostUnload += (sender, e) => OnHostUnload (
				(IApplicationHost)sender, 
				e.IsShutdown);

			TransportConfig config = transportConfig as TransportConfig;

			if (config != null) {
				mt=config.MultiThreading;
			}
		}

		public virtual void CreateRequest (ulong requestId, int requestNumber)
		{
			//StackTrace stackTrace = new StackTrace();
			//MethodBase methodBase = stackTrace.GetFrame(1).GetMethod();
			Console.WriteLine ("CreateRequest: reqId={0}, reqN={1}",requestId ,requestNumber);
			//Console.WriteLine("StackTrace: '{0}'", Environment.StackTrace);
			IWebRequest req=AppHost.CreateRequest (requestId, requestNumber, null);

			lock (requestsLock) {
				requests.Add (requestId, req);
			}
			Console.WriteLine ("CreateRequest: requests.Added. reqId={0}, reqN={1}",requestId ,requestNumber);


		}

		public virtual void AddServerVariable (ulong requestId, int requestNumber, string name, string value)
		{
			IWebRequest request;
			lock (requestsLock)
			{
				requests.TryGetValue (requestId, out request);
			}

			try
			{
				if (request != null 
					&& request.RequestNumber == requestNumber) {
					request.AddServerVariable (name, value);
				}
			} catch (Exception ex) {
				Console.WriteLine ("ex={0}", ex.ToString ());
			}
		}

		public virtual void AddHeader (ulong requestId, int requestNumber, string name, string value)
		{
			IWebRequest request;
			lock (requestsLock)
			{
				requests.TryGetValue (requestId, out request);
			}

			if (request != null
				&& request.RequestNumber == requestNumber) {
				request.AddHeader (name, value);
			}
		}

		public virtual void HeadersSent (ulong requestId, int requestNumber)
		{

		}

		public virtual void AddBodyPart (ulong requestId, int requestNumber, byte[] body, bool final)
		{
			Console.WriteLine ("AddBodyPart");
			IWebRequest request;

			lock (requestsLock)
			{
				requests.TryGetValue (requestId, out request);
			}
			Console.WriteLine ("AddBodyPart - past lock 1");
			if (request == null) {
				Console.WriteLine ("AddBodyPart - request is null for requestId:{0}, requestNumber{1}", requestId, requestNumber);
				return;
			}
			if (request.RequestNumber != requestNumber) {
				Console.WriteLine ("AddBodyPart - request.RequestNumber:{0} != requestNumber{1}. For request.requestId:{2} and requestId:{3}", request.RequestNumber, requestNumber, request.RequestId, requestId);
				return;
			}

				if (final) {
					Console.WriteLine ("AddBodyPart - enter lock 2");
					lock (requestsLock) {
						requests.Remove (requestId);
					}
					Console.WriteLine ("AddBodyPart - past lock 2");
					switch (mt) {
					case MultiThreadingOption.Single:
						request.Process ((IWebResponse)request);
						break;
					case MultiThreadingOption.Task:
						#if !NET_2_0
						Task.Factory.StartNew (() => {
							request.Process ((IWebResponse)request);
						});
						break;
						#endif
					case MultiThreadingOption.ThreadPool:
					default:
						ThreadPool.QueueUserWorkItem (_ => request.Process ((IWebResponse)request));
						break;
					}
				} else {
					request.AddBodyPart (body);
				}
			

		}

		public virtual void Process (ulong requestId, int requestNumber)
		{
			Console.WriteLine ("Remove ReqId={0}", requestId);
			lock (requestsLock) {
				requests.Remove (requestId);
			}
		}

		public abstract void SendOutput (ulong requestId, int requestNumber, byte[] data, int len);

		public abstract void EndRequest (ulong requestId, int requestNumber, int appStatus);

		#endregion
	}
}

